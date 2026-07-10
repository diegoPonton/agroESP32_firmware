#pragma once
// =====================================================
// Nodo emisor — Seeed XIAO ESP32-S3 + RFM95W (módulo externo por SPI)
// Único sensor: BME280 (ambiente: temp/hum/presión) por I2C.
// Mismo perfil de radio que el emisor DOIT para que el receptor
// existente lo reciba sin cambios.
// =====================================================

// --- LoRa SPI (RFM95W, 915 MHz) ---
#define LORA_CS_PIN    D3
#define LORA_RST_PIN   D1
#define LORA_DIO0_PIN  D2
#define LORA_SCK_PIN   D8
#define LORA_MISO_PIN  D9
#define LORA_MOSI_PIN  D10
#define LORA_FREQ_MHZ  915.0f
// SF: 0 = default RadioLib (SF7). Cambiar solo si el receptor usa el mismo SF.
#define LORA_SF_DEFAULT   0
#define LORA_TX_DBM       20
#define LORA_SYNC_WORD    0x12   // 0x12 = red privada, 0x34 = LoRaWAN público
#define CMD_WINDOW_MS      7000  // ventana de escucha LoRa tras cada TX (ms)

// --- I2C (BME280) ---
#define I2C_SDA_PIN    D4
#define I2C_SCL_PIN    D5
#define BME280_I2C_ADDR 0x76   // 0x76 en la mayoría de los breakouts; 0x77 en algunos

// --- Comportamiento ---
#define DEFAULT_SLEEP_MIN  10
