// Arquivo: aht10.c
#include "aht10.h"
#include <stdio.h>

// Comandos do AHT10
const uint8_t CMD_RESET[] = {0xBA};
const uint8_t CMD_INIT[] = {0xE1, 0x08, 0x00};
const uint8_t CMD_MEASURE[] = {0xAC, 0x33, 0x00};

bool aht10_init(i2c_inst_t* i2c) {
    // 1. Soft Reset: Ajuda a "acordar" o sensor se ele estiver em um estado estranho.
    i2c_write_blocking(i2c, AHT10_ADDR, CMD_RESET, sizeof(CMD_RESET), false);
    sleep_ms(20);

    // 2. Envia o comando de inicialização com calibração
    if (i2c_write_blocking(i2c, AHT10_ADDR, CMD_INIT, sizeof(CMD_INIT), false) < 0) {
        printf("ERRO: Falha ao enviar comando de init para o AHT10.\n");
        return false;
    }
    sleep_ms(40); // Espera um pouco após a inicialização

    // 3. Verifica o status para garantir que a calibração foi habilitada
    uint8_t status;
    if (i2c_read_blocking(i2c, AHT10_ADDR, &status, 1, false) < 0) {
        printf("ERRO: Falha ao ler status do AHT10 na inicialização.\n");
        return false;
    }

    // O bit 3 (0x08) indica que o sensor está calibrado.
    if ((status & 0x08) == 0) {
        printf("ERRO: AHT10 não conseguiu se calibrar.\n");
        return false;
    }
    
    return true;
}

bool aht10_read_data(i2c_inst_t* i2c, aht10_data_t* data) {
    // 1. Envia o comando para disparar uma medição
    if (i2c_write_blocking(i2c, AHT10_ADDR, CMD_MEASURE, sizeof(CMD_MEASURE), false) < 0) {
        return false;
    }

    // 2. Espera o tempo de medição (datasheet diz ~75ms, 80ms é seguro)
    sleep_ms(80);

    // 3. Lê os 6 bytes de dados de resposta
    uint8_t buf[6];
    if (i2c_read_blocking(i2c, AHT10_ADDR, buf, sizeof(buf), false) < 0) {
        return false;
    }

    // 4. Checa o byte de status para ver se o sensor está ocupado
    // O bit 7 (0x80) deve ser 0 para indicar que a medição está pronta.
    if ((buf[0] & 0x80) != 0) {
        // O sensor ainda está ocupado, tente novamente mais tarde.
        return false;
    }

    // 5. Calcula os valores de umidade e temperatura com base nas fórmulas do datasheet
    uint32_t raw_humidity = ((uint32_t)(buf[1]) << 12) | ((uint32_t)(buf[2]) << 4) | (buf[3] >> 4);
    data->humidity = ((float)raw_humidity / 1048576.0f) * 100.0f;

    uint32_t raw_temp = (((uint32_t)buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
    data->temperature = (((float)raw_temp / 1048576.0f) * 200.0f) - 50.0f;

    return true;
}