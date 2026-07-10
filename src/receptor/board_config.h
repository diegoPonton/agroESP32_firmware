#pragma once
// =====================================================
// GENERADO AUTOMÁTICAMENTE por AgroESP32 Platform
// Modificar desde la plataforma web — no editar a mano
// =====================================================

// --- LoRa SPI ---
#define LORA_CS_PIN    25
#define LORA_RST_PIN   14
#define LORA_DIO0_PIN  26
#define LORA_SCK_PIN   18
#define LORA_MISO_PIN  19
#define LORA_MOSI_PIN  23
#define LORA_FREQ_MHZ  915.0f
#define LORA_TX_DBM    13
#define LORA_SYNC_WORD 0x12   // debe coincidir con los emisores

// --- Comportamiento red ---
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WIFI_RETRY_MS            10000
#define HTTP_POST_TIMEOUT_MS     8000
#define HTTP_RELAY_TIMEOUT_MS    4000
#define HTTP_POST_RETRIES        3      // reintentos si POST falla
#define HTTP_RETRY_DELAY_MS      2000
