#pragma once
// =====================================================
// GENERADO AUTOMÁTICAMENTE por AgroESP32 Platform
// Modificar desde la plataforma web — no editar a mano
// =====================================================

// --- LoRa SPI (RFM95W, 915 MHz) ---
#define LORA_CS_PIN    25
#define LORA_RST_PIN   14
#define LORA_DIO0_PIN  26
#define LORA_SCK_PIN   18
#define LORA_MISO_PIN  19
#define LORA_MOSI_PIN  23
#define LORA_FREQ_MHZ  915.0f
// SF: 0 = default RadioLib (SF7). Cambiar solo si el receptor usa el mismo SF.
#define LORA_SF_DEFAULT   0
#define LORA_TX_DBM       20
#define LORA_SYNC_WORD    0x12   // 0x12 = red privada, 0x34 = LoRaWAN público
#define CMD_WINDOW_MS      7000  // ventana de escucha LoRa tras cada TX (ms)

// --- I2C (sensor ambiental) ---
#define I2C_SDA_PIN    21
#define I2C_SCL_PIN    22

// --- DS18B20 OneWire (sonda de suelo) ---
#define DS18B20_PIN        4
#define DS18B20_RESOLUTION 12   // bits (9-12)

// --- GPS UART ---
#define GPS_RX_PIN          16
#define GPS_TX_PIN          17
#define GPS_BAUD            9600
#define GPS_TIMEOUT_COLD_S  60
#define GPS_TIMEOUT_WARM_S  30
#define GPS_MIN_SATS        3

// --- Batería ADC ---
#define BAT_ADC_PIN    34
#define BAT_VREF       3.3f
#define BAT_ADC_MAX    4095
#define BAT_DIV_RATIO  2.0f    // divisor resistivo (R1=R2 → ratio 2.0)
#define BAT_SAMPLES    50
#define BAT_V_MAX      4.20f   // Li-Ion: 4.2  |  LiFePO4: 3.65
#define BAT_V_MIN      3.20f   // por debajo de esto = 0%

// --- Sensores habilitados por defecto (sobreescribible vía NVS/comando remoto) ---
// Ambiente: SHTC3 integrado en la placa GY-39 (HiLetgo), leído directo por
// su dirección I2C estándar 0x70. El SPL06-007 (presión) de esa misma
// placa no está soportado — no es compatible con el driver BME280.
#define SENSOR_SHTC3_DEFAULT    true
#define SENSOR_DS18B20_DEFAULT  true
#define SENSOR_GPS_DEFAULT      true

// --- Comportamiento ---
#define DEFAULT_SLEEP_MIN  10
