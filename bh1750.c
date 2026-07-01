/**
 * @file bh1750.c
 * @brief Driver para o sensor de luminosidade I2C BH1750.
 */

#include "pico/stdlib.h"
#include "bh1750.h"

// Endereço e comandos do sensor, agora encapsulados neste arquivo.
const uint8_t BH1750_ADDR = 0x23;
const uint8_t BH1750_CMD_POWER_ON = 0x01;
const uint8_t BH1750_CMD_RESET = 0x07;
const uint8_t BH1750_CMD_HIRES1 = 0x20; // Modo de alta resolução (1 lux)

void bh1750_init(i2c_inst_t *i2c) {
    // Envia a sequência de inicialização para o sensor
    i2c_write_blocking(i2c, BH1750_ADDR, &BH1750_CMD_POWER_ON, 1, false);
    sleep_ms(10);
    i2c_write_blocking(i2c, BH1750_ADDR, &BH1750_CMD_RESET, 1, false);
    sleep_ms(10);
}

float bh1750_read_lux(i2c_inst_t *i2c) {
    uint8_t raw_data[2];

    // Envia o comando para iniciar uma nova medição.
    i2c_write_blocking(i2c, BH1750_ADDR, &BH1750_CMD_HIRES1, 1, false);
    
    // Aguarda o tempo de conversão do sensor (180ms para alta resolução).
    sleep_ms(180);
    
    // Lê os 2 bytes do resultado.
    int bytes_read = i2c_read_blocking(i2c, BH1750_ADDR, raw_data, 2, false);
    if (bytes_read < 2) {
        return -1.0f; // Retorna erro se a leitura falhar.
    }
    
    // Combina os bytes e aplica a fórmula de conversão do datasheet.
    uint16_t raw_value = (raw_data[0] << 8) | raw_data[1];
    return (float)raw_value / 1.2f;
}