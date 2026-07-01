/*
 * =============================================
 * Projeto: Web Server para Monitoramento Ambiental com Pico W
 * =============================================
 * O projeto para iniciar deve ter no minimo UM SENSOR I2C CONECTADO NO I2C1
 * pois o sistema faz uma validação de inicialização
 * e se não houver nenhum sensor conectado, o sistema não inicia.
 * A conexão com a rede WiFi é obrigatória para o funcionamento do servidor web.
 * deve ser configurada nas constantes WIFI_SSID e WIFI_PASSWORD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "bh1750.h"
#include "aht10.h"

#define WIFI_SSID "M20"
#define WIFI_PASSWORD "11111111"

// Pinos
#define I2C_PORT i2c1
#define I2C_SDA_PIN 2
#define I2C_SCL_PIN 3
#define PIR_GPIO 16
#define BUZZER_GPIO 21
#define RGB_RED_PIN 11
#define RGB_GREEN_PIN 12
#define RGB_BLUE_PIN 13
#define TCP_PORT 80
#define BMP280_ADDR 0x76

// <-- ALTERAÇÃO: Configuração do Data Logger
#define DATA_LOG_SIZE 200 // Armazena as últimas 200 leituras (aprox. 6.5 minutos de dados)

// Limites dos alarmes
static const float THRESHOLD_TEMP_MAX = 30.0f;
static const float THRESHOLD_TEMP_MIN = 18.0f;
static const float THRESHOLD_HUMIDITY_MAX = 72.0f;
static const float THRESHOLD_HUMIDITY_MIN = 64.0f;

// Timings e Constantes
#define SENSOR_POLL_TIME_MS 2000
#define HPA_TO_ATM 0.000986923f
#define ALARM_FREQ 988
#define ALARM_TEMP_BEEP_MS 200
#define ALARM_HUM_BEEP_MS 1000

typedef enum
{
    ALARM_STATE_OFF,
    ALARM_STATE_MOTION,
    ALARM_STATE_TEMP,
    ALARM_STATE_HUMIDITY
} alarm_state_t;
typedef struct
{
    uint slice_num;
    uint chan_num;
} pwm_chan_t;

// <-- ALTERAÇÃO: Estrutura para armazenar cada entrada do log
typedef struct
{
    uint32_t timestamp_s;
    float temperature;
    float pressure;
    float humidity;
    float luminosity;
    bool motion;
} data_log_entry_t;

// --- Variáveis Globais ---
static float current_temperature = 0.0f, current_pressure = 0.0f, current_luminosity = 0.0f, current_humidity = 0.0f;
static bool current_motion = false, sensor_ready = false, bmp280_ready = false, aht10_ready = false;
static volatile bool alarm_armed = true;

// Visibilidade controla exibição e alarmes
static volatile bool show_temperature = true, show_pressure = true, show_humidity = true, show_luminosity = true, show_motion = true;

// Estado dos alarmes
static alarm_state_t current_alarm_state = ALARM_STATE_OFF;
static bool is_beep_on = false, is_siren_tone_high = false;
static uint pwm_buzzer_slice_num;
static int motion_blink_counter = 0;
pwm_chan_t pwm_r, pwm_g, pwm_b;

// <-- ALTERAÇÃO: Buffer circular e índice para o log de dados
static data_log_entry_t data_log[DATA_LOG_SIZE];
static int data_log_index = 0;
static bool data_log_full = false;

// --- Funções de Hardware e Rede (sem alterações, omitidas por brevidade) ---
static bool bmp_safe_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (i2c_write_blocking(I2C_PORT, BMP280_ADDR, &reg, 1, true) != 1)
        return false;
    return i2c_read_blocking(I2C_PORT, BMP280_ADDR, buf, len, false) == len;
}
static void bmp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(I2C_PORT, BMP280_ADDR, buf, 2, false);
}
static int32_t t_fine;
static uint16_t dig_T1;
static int16_t dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static uint16_t read_u16(uint8_t reg)
{
    uint8_t buf[2];
    if (bmp_safe_read(reg, buf, 2))
    {
        return (buf[1] << 8) | buf[0];
    }
    return 0;
}
static int16_t read_s16(uint8_t reg) { return (int16_t)read_u16(reg); }
static void bmp280_read_calibration()
{
    dig_T1 = read_u16(0x88);
    dig_T2 = read_s16(0x8A);
    dig_T3 = read_s16(0x8C);
    dig_P1 = read_u16(0x8E);
    dig_P2 = read_s16(0x90);
    dig_P3 = read_s16(0x92);
    dig_P4 = read_s16(0x94);
    dig_P5 = read_s16(0x96);
    dig_P6 = read_s16(0x98);
    dig_P7 = read_s16(0x9A);
    dig_P8 = read_s16(0x9C);
    dig_P9 = read_s16(0x9E);
}
static bool bmp280_init()
{
    uint8_t chip_id;
    if (!bmp_safe_read(0xD0, &chip_id, 1) || chip_id != 0x58)
    {
        printf("Erro: BMP280 nao detectado.\n");
        return false;
    }
    printf("BMP280 detectado.\n");
    bmp_write(0xE0, 0xB6);
    sleep_ms(10);
    bmp_write(0xF4, 0x27);
    bmp_write(0xF5, 0xA0);
    bmp280_read_calibration();
    return true;
}
static int32_t bmp280_compensate_T(int32_t adc_T)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;
}
static uint32_t bmp280_compensate_P(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0)
        return 0;
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)p / 256;
}
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err); // Fwd decl
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL)
        return ERR_VAL;
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}
static bool tcp_server_init()
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb)
        return false;
    if (tcp_bind(pcb, NULL, TCP_PORT) != ERR_OK)
    {
        tcp_abort(pcb);
        return false;
    }
    struct tcp_pcb *listen_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!listen_pcb)
    {
        tcp_abort(pcb);
        return false;
    }
    tcp_accept(listen_pcb, tcp_server_accept);
    printf("Servidor TCP iniciado na porta %d\n", TCP_PORT);
    return true;
}
static bool init_wifi()
{
    if (cyw43_arch_init())
        return false;
    cyw43_arch_enable_sta_mode();
    printf("Conectando ao WiFi: %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000) == 0)
    {
        printf("WiFi conectado!\n");
        return true;
    }
    printf("Falha na conexao WiFi\n");
    return false;
}
void set_led_color_pwm(uint8_t r, uint8_t g, uint8_t b)
{
    pwm_set_chan_level(pwm_r.slice_num, pwm_r.chan_num, r * r);
    pwm_set_chan_level(pwm_g.slice_num, pwm_g.chan_num, g * g);
    pwm_set_chan_level(pwm_b.slice_num, pwm_b.chan_num, b * b);
}
void init_pwm_rgb_led()
{
    gpio_set_function(RGB_RED_PIN, GPIO_FUNC_PWM);
    gpio_set_function(RGB_GREEN_PIN, GPIO_FUNC_PWM);
    gpio_set_function(RGB_BLUE_PIN, GPIO_FUNC_PWM);
    pwm_r.slice_num = pwm_gpio_to_slice_num(RGB_RED_PIN);
    pwm_r.chan_num = pwm_gpio_to_channel(RGB_RED_PIN);
    pwm_g.slice_num = pwm_gpio_to_slice_num(RGB_GREEN_PIN);
    pwm_g.chan_num = pwm_gpio_to_channel(RGB_GREEN_PIN);
    pwm_b.slice_num = pwm_gpio_to_slice_num(RGB_BLUE_PIN);
    pwm_b.chan_num = pwm_gpio_to_channel(RGB_BLUE_PIN);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 65535);
    pwm_init(pwm_r.slice_num, &config, true);
    pwm_init(pwm_g.slice_num, &config, true);
    pwm_init(pwm_b.slice_num, &config, true);
    set_led_color_pwm(0, 0, 0);
}
void set_buzzer_freq(uint freq)
{
    if (freq == 0)
    {
        pwm_set_gpio_level(BUZZER_GPIO, 0);
        return;
    }
    uint32_t wrap = 1000;
    float div = (float)clock_get_hz(clk_sys) / (wrap * freq);
    pwm_set_clkdiv(pwm_buzzer_slice_num, div);
    pwm_set_wrap(pwm_buzzer_slice_num, wrap - 1);
    pwm_set_gpio_level(BUZZER_GPIO, wrap / 2);
}
void init_pwm_buzzer()
{
    gpio_set_function(BUZZER_GPIO, GPIO_FUNC_PWM);
    pwm_buzzer_slice_num = pwm_gpio_to_slice_num(BUZZER_GPIO);
    pwm_config config = pwm_get_default_config();
    pwm_init(pwm_buzzer_slice_num, &config, true);
    set_buzzer_freq(0);
}

void read_and_update_state()
{
    // Leitura dos sensores
    if (bmp280_ready)
    {
        uint8_t data[6];
        if (bmp_safe_read(0xF7, data, 6))
        {
            int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
            int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
            current_temperature = bmp280_compensate_T(adc_T) / 100.0f;
            float pressure_hpa = bmp280_compensate_P(adc_P) / 100.0f;
            current_pressure = pressure_hpa * HPA_TO_ATM;
        }
    }
    current_luminosity = bh1750_read_lux(I2C_PORT);
    if (aht10_ready)
    {
        aht10_data_t aht_data;
        if (aht10_read_data(I2C_PORT, &aht_data))
        {
            current_humidity = aht_data.humidity;
        }
    }
    current_motion = gpio_get(PIR_GPIO);

    // <-- ALTERAÇÃO: Salva a nova leitura no buffer circular
    data_log[data_log_index] = (data_log_entry_t){
        .timestamp_s = to_ms_since_boot(get_absolute_time()) / 1000,
        .temperature = current_temperature,
        .pressure = current_pressure,
        .humidity = current_humidity,
        .luminosity = current_luminosity,
        .motion = current_motion};
    data_log_index++;
    if (data_log_index >= DATA_LOG_SIZE)
    {
        data_log_index = 0;
        data_log_full = true;
    }

    // Lógica dos alarmes
    bool temp_out_of_range = current_temperature > THRESHOLD_TEMP_MAX || current_temperature < THRESHOLD_TEMP_MIN;
    bool hum_out_of_range = current_humidity > THRESHOLD_HUMIDITY_MAX || current_humidity < THRESHOLD_HUMIDITY_MIN;

    if (alarm_armed && show_motion && current_motion)
    {
        current_alarm_state = ALARM_STATE_MOTION;
    }
    else if (alarm_armed && show_temperature && temp_out_of_range)
    {
        current_alarm_state = ALARM_STATE_TEMP;
        set_led_color_pwm(255, 0, 0);
    }
    else if (alarm_armed && show_humidity && hum_out_of_range)
    {
        current_alarm_state = ALARM_STATE_HUMIDITY;
        set_led_color_pwm(255, 0, 0);
    }
    else
    {
        current_alarm_state = ALARM_STATE_OFF;
        set_led_color_pwm(0, 0, 0);
    }
}

// --- Lógica do Servidor Web ---
#define SEND_TCP_STRING(tpcb, str) tcp_write(tpcb, str, strlen(str), TCP_WRITE_FLAG_COPY)

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (p == NULL)
    {
        tcp_close(tpcb);
        return ERR_OK;
    }

    char *payload = (char *)p->payload;
    const char *http_redirect = "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n";

    if (strncmp(payload, "GET /toggle-alarm", 17) == 0)
    {
        alarm_armed = !alarm_armed;
        if (!alarm_armed)
        {
            current_alarm_state = ALARM_STATE_OFF;
            set_buzzer_freq(0);
            set_led_color_pwm(0, 0, 0);
        }
        SEND_TCP_STRING(tpcb, http_redirect);
    }
    else if (strncmp(payload, "GET /toggle-vis?sensor=", 23) == 0)
    {
        const char *sensor_id = payload + 23;
        if (strncmp(sensor_id, "temp", 4) == 0)
            show_temperature = !show_temperature;
        else if (strncmp(sensor_id, "press", 5) == 0)
            show_pressure = !show_pressure;
        else if (strncmp(sensor_id, "hum", 3) == 0)
            show_humidity = !show_humidity;
        else if (strncmp(sensor_id, "lum", 3) == 0)
            show_luminosity = !show_luminosity;
        else if (strncmp(sensor_id, "motion", 6) == 0)
            show_motion = !show_motion;
        SEND_TCP_STRING(tpcb, http_redirect);
    }
    // <-- ALTERAÇÃO: Novo endpoint para servir o arquivo CSV
    else if (strncmp(payload, "GET /download", 13) == 0)
    {
        // Envia cabeçalhos HTTP que forçam o download do arquivo
        SEND_TCP_STRING(tpcb, "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/csv\r\n"
                              "Content-Disposition: attachment; filename=\"datalog.csv\"\r\n\r\n");

        // Envia a linha do cabeçalho do CSV
        SEND_TCP_STRING(tpcb, "Timestamp(s),Temperatura(C),Pressao(atm),Umidade(%),Luminosidade(lux),Movimento\r\n");

        char csv_line[128];
        int start_index = 0;
        int end_index = data_log_index;

        if (data_log_full)
        {
            start_index = data_log_index;
            end_index = DATA_LOG_SIZE;
        }

        // Envia as linhas de dados, tratando o buffer circular
        for (int i = start_index; i < end_index; i++)
        {
            snprintf(csv_line, sizeof(csv_line), "%lu,%.1f,%.4f,%.1f,%.0f,%s\r\n",
                     data_log[i].timestamp_s, data_log[i].temperature, data_log[i].pressure,
                     data_log[i].humidity, data_log[i].luminosity, data_log[i].motion ? "Sim" : "Nao");
            SEND_TCP_STRING(tpcb, csv_line);
        }
        if (data_log_full)
        {
            for (int i = 0; i < data_log_index; i++)
            {
                snprintf(csv_line, sizeof(csv_line), "%lu,%.1f,%.4f,%.1f,%.0f,%s\r\n",
                         data_log[i].timestamp_s, data_log[i].temperature, data_log[i].pressure,
                         data_log[i].humidity, data_log[i].luminosity, data_log[i].motion ? "Sim" : "Nao");
                SEND_TCP_STRING(tpcb, csv_line);
            }
        }
    }
    else if (strncmp(payload, "GET / HTTP", 10) == 0)
    {
        SEND_TCP_STRING(tpcb, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n");
        SEND_TCP_STRING(tpcb, "<!DOCTYPE html><html><head><meta charset=UTF-8><title>Monitor</title><style>"
                              "*{box-sizing:border-box;margin:0;padding:0}body{font-family:sans-serif;background-color:#161c29;color:#f0f0f0;text-align:center;padding:1rem}h1{margin-bottom:.5rem}"
                              ".grid{display:flex;flex-wrap:wrap;justify-content:center;gap:15px;margin:1rem auto}"
                              ".card{background:rgba(35,43,60,.6);border:1px solid rgba(255,255,255,.1);border-radius:12px;padding:15px;width:220px}"
                              ".card h3{font-size:1.1rem;font-weight:400;margin-bottom:10px;opacity:.9}.value{font-size:2.8rem;font-weight:700;line-height:1.2;margin:10px 0}"
                              ".unit{font-size:.9rem;opacity:.7}.temp .value{color:#ff8a80}.pressure .value{color:#69f0ae}.humidity .value{color:#40c4ff}.light .value{color:#ffd740}"
                              ".motion .value{font-size:1.5rem;padding:12px 0}.motion.active{background:rgba(255,100,100,.3)}"
                              ".controls{margin:1rem auto;padding:1rem;background:rgba(35,43,60,.4);border-radius:12px;max-width:95%}"
                              "h2{font-size:1.1rem;margin-bottom:.5rem}.button{padding:8px 16px;font-size:.9rem;color:#fff;text-decoration:none;border-radius:8px;margin:4px}"
                              ".vis-toggle.visible{background-color:rgba(76,175,80,.4)}.vis-toggle.hidden{background-color:rgba(158,158,158,.3);opacity:.7}"
                              "</style><script>setTimeout(()=>window.location.reload(),2000)</script></head><body><h1>Dashboard</h1><div class=grid>");

        char card_buffer[256];
        if (show_temperature)
        {
            // Unicode: U+F2C1 () -> HTML: &#xF2C1;
            snprintf(card_buffer, sizeof(card_buffer), "<div class=\"card temp\"><h3>Temp</h3><div class=value>%.1f</div><div class=unit>°C</div></div>", current_temperature);
            SEND_TCP_STRING(tpcb, card_buffer);
        }
        if (show_pressure)
        {
            // Unicode: U+F4A8 () -> HTML: &#xF4A8;
            snprintf(card_buffer, sizeof(card_buffer), "<div class=\"card pressure\"><h3>Pressão</h3><div class=value>%.4f</div><div class=unit>atm</div></div>", current_pressure);
            SEND_TCP_STRING(tpcb, card_buffer);
        }
        if (show_humidity)
        {
            // Unicode: U+F4A7 () -> HTML: &#xF4A7;
            snprintf(card_buffer, sizeof(card_buffer), "<div class=\"card humidity\"><h3>Umidade</h3><div class=value>%.1f</div><div class=unit>%%</div></div>", current_humidity);
            SEND_TCP_STRING(tpcb, card_buffer);
        }
        if (show_luminosity)
        {
            // Unicode: U+F4A1 () -> HTML: &#xF4A1;
            snprintf(card_buffer, sizeof(card_buffer), "<div class=\"card light\"><h3>Luz</h3><div class=value>%.0f</div><div class=unit>lux</div></div>", current_luminosity);
            SEND_TCP_STRING(tpcb, card_buffer);
        }
        if (show_motion)
        {
            // Unicode: U+F6B6 () -> HTML: &#xF6B6;
            snprintf(card_buffer, sizeof(card_buffer), "<div class=\"card motion %s\"><h3>Movim.</h3><div class=value>%s</div></div>", current_motion ? "active" : "", current_motion ? "MOVIMENTO DETECTADO" : "SEM MOVIMENTO");
            SEND_TCP_STRING(tpcb, card_buffer);
        }

        char controls_buffer[512];
        snprintf(controls_buffer, sizeof(controls_buffer), "</div><div class=controls><h2>Exibir / Habilitar Alarmes</h2>"
                                                           "<a href=\"/toggle-vis?sensor=temp\" class=\"button vis-toggle %s\">Temperatura</a>"
                                                           "<a href=\"/toggle-vis?sensor=hum\" class=\"button vis-toggle %s\">Umidade</a>"
                                                           "<a href=\"/toggle-vis?sensor=press\" class=\"button vis-toggle %s\">Pressão</a>"
                                                           "<a href=\"/toggle-vis?sensor=lum\" class=\"button vis-toggle %s\">Luz</a>"
                                                           "<a href=\"/toggle-vis?sensor=motion\" class=\"button vis-toggle %s\">Movimento</a></div>",
                 show_temperature ? "visible" : "hidden", show_humidity ? "visible" : "hidden",
                 show_pressure ? "visible" : "hidden", show_luminosity ? "visible" : "hidden", show_motion ? "visible" : "hidden");
        SEND_TCP_STRING(tpcb, controls_buffer);

        // <-- ALTERAÇÃO: Adicionado o botão de Download
        char final_controls[512];
        snprintf(final_controls, sizeof(final_controls),
                 "<div class=controls style=margin-top:1rem><h2>Controles</h2>"
                 "<a href=/toggle-alarm class=button style=background-color:%s>%s</a>"
                 "<a href=/download class=button style=background-color:#0277bd>⬇ Baixar CSV</a>"
                 "</div></body></html>",
                 alarm_armed ? "#c62828" : "#2e7d32", alarm_armed ? "Desarmar" : "Rearmar");
        SEND_TCP_STRING(tpcb, final_controls);
    }

    tcp_recved(tpcb, p->tot_len);
    tcp_output(tpcb);
    pbuf_free(p);
    tcp_close(tpcb);
    return ERR_OK;
}

// --- Função Principal ---
int main()
{
    stdio_init_all();
    sleep_ms(2000);
    if (!init_wifi())
    {
        while (true)
            ;
    }

    // Init Hardware
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_init(PIR_GPIO);
    gpio_set_dir(PIR_GPIO, GPIO_IN);
    init_pwm_buzzer();
    init_pwm_rgb_led();

    // Init Sensores
    bmp280_ready = bmp280_init();
    bh1750_init(I2C_PORT);
    aht10_ready = aht10_init(I2C_PORT);
    sensor_ready = true;
    printf("Sistema iniciado!\n");
    if (!tcp_server_init())
    {
        while (true)
            ;
    }

    struct netif *netif = netif_default;
    if (netif && netif_is_up(netif))
    {
        printf("\n=== SERVIDOR ATIVO em http://%s ===\n", ip4addr_ntoa(&netif->ip_addr));
    }

    absolute_time_t next_sensor_read = make_timeout_time_ms(0);
    absolute_time_t next_alarm_update = make_timeout_time_ms(0);

    while (true)
    {
        cyw43_arch_poll();

        if (absolute_time_diff_us(get_absolute_time(), next_sensor_read) < 0)
        {
            read_and_update_state();
            next_sensor_read = make_timeout_time_ms(SENSOR_POLL_TIME_MS);
        }

        if (current_alarm_state != ALARM_STATE_OFF)
        {
            if (absolute_time_diff_us(get_absolute_time(), next_alarm_update) < 0)
            {
                switch (current_alarm_state)
                {
                case ALARM_STATE_MOTION:
                    is_siren_tone_high = !is_siren_tone_high;
                    set_buzzer_freq(is_siren_tone_high ? 880 : 440);
                    motion_blink_counter++;
                    if (motion_blink_counter < 2)
                    {
                        set_led_color_pwm(255, 0, 0);
                    }
                    else
                    {
                        set_led_color_pwm(0, 0, 0);
                    }
                    if (motion_blink_counter >= 4)
                    {
                        motion_blink_counter = 0;
                    }
                    next_alarm_update = make_timeout_time_ms(250);
                    break;
                case ALARM_STATE_TEMP:
                    is_beep_on = !is_beep_on;
                    set_buzzer_freq(is_beep_on ? ALARM_FREQ : 0);
                    next_alarm_update = make_timeout_time_ms(ALARM_TEMP_BEEP_MS / 2);
                    break;
                case ALARM_STATE_HUMIDITY:
                    is_beep_on = !is_beep_on;
                    set_buzzer_freq(is_beep_on ? ALARM_FREQ : 0);
                    next_alarm_update = make_timeout_time_ms(ALARM_HUM_BEEP_MS / 2);
                    break;
                default:
                    set_buzzer_freq(0);
                    break;
                }
            }
        }
        else
        {
            set_buzzer_freq(0);
            is_beep_on = false;
            motion_blink_counter = 0;
        }
    }
    return 0;
}