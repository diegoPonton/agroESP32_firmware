// =====================================================
// EMISOR XIAO (NODO SENSOR) -> LoRa (RFM95W) -> RECEPTOR DOIT
// Seeed XIAO ESP32-S3 + BME280 (único sensor: ambiente) -> JSON -> LoRa
//
// Mismo protocolo que el emisor DOIT (src/emisor/main.cpp): payload
// JSON con "device_id"/"amb", ventana de comandos tras cada TX,
// config por NVS/secrets.h/provisioning por serial. El receptor
// existente no necesita cambios — solo le llega un paquete con
// menos campos (sin "probe"/"gps"/"bat").
// =====================================================
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>

#include "board_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// =====================
// Config global
// =====================
String g_device_id  = "";
int    g_sleep_min  = DEFAULT_SLEEP_MIN;
bool   g_relay_on   = false;

// =====================
// Tipos
// =====================
struct THReading { float temp_c; float hum_pct; float pressure_hpa; bool ok; };

// =====================
// Hardware
// =====================
Adafruit_BME280 bme;
bool bmeFound = false;

// =====================
// Relevador (SSR en RELAY_PIN)
// =====================
void applyRelay(bool on) {
  g_relay_on = on;
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

// =====================
// LoRa (RadioLib — ver nota en src/emisor/main.cpp sobre RadioHead vs RadioLib)
// =====================
Module loraModule(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, RADIOLIB_NC, SPI, SPISettings(500000, MSBFIRST, SPI_MODE0));
SX1276 radio(&loraModule);
const int MAX_LORA = 250;

bool initLoRa() {
  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);

  uint8_t sf = (LORA_SF_DEFAULT > 0) ? LORA_SF_DEFAULT : 7;
  int state = radio.begin(LORA_FREQ_MHZ, 125.0, sf, 5, LORA_SYNC_WORD, LORA_TX_DBM, 8, 0);
  return state == RADIOLIB_ERR_NONE;
}

// =====================
// Prototipos
// =====================
THReading readBME280();
void buildJsonPayload(String &out, const THReading& amb);
void processPacket(const String& raw);
void processCommand(const String& rawCmd);
void finishCycle();
void enterProvisioningMode();
void loadConfig();

// =====================
// Carga de configuración
// =====================
void loadConfig() {
  Preferences prefs;
  prefs.begin("agro", true);
  String nvsId = prefs.getString("device_id", "");
  g_relay_on = prefs.getBool("relay_on", false);
  if (nvsId.length() > 0) {
    g_device_id = nvsId;
    g_sleep_min = prefs.getInt("sleep_min", DEFAULT_SLEEP_MIN);
    prefs.end();
    Serial.println("[CONFIG] NVS OK");
    return;
  }
  prefs.end();

#ifdef DEVICE_ID_SECRET
  g_device_id = DEVICE_ID_SECRET;
  g_sleep_min = SLEEP_MINUTES;
  Serial.printf("[CONFIG] secrets.h — %s\n", g_device_id.c_str());
  return;
#endif

  enterProvisioningMode();
}

// =====================
// Modo provisioning
// =====================
void enterProvisioningMode() {
  Serial.println("[PROV] Sin configuracion. Modo provisioning activo.");
  String incoming = "";
  unsigned long deadline = millis() + 90000UL;
  unsigned long lastReady = 0;
  while (millis() < deadline) {
    if (millis() - lastReady >= 1000) { Serial.println("PROV_READY"); lastReady = millis(); }
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        incoming.trim();
        if (incoming.length() > 5) {
          StaticJsonDocument<256> cfg;
          if (!deserializeJson(cfg, incoming)) {
            const char* id  = cfg["device_id"];
            int sleepMin    = cfg["sleep_min"] | DEFAULT_SLEEP_MIN;
            if (id && strlen(id) > 0) {
              Preferences prefs; prefs.begin("agro", false);
              prefs.putString("device_id", id);
              prefs.putInt("sleep_min", sleepMin);
              prefs.end();
              Serial.println("PROV_OK");
              delay(200); ESP.restart();
            }
          }
        }
        incoming = "";
      } else if (c >= 32) { incoming += c; }
    }
    delay(10);
  }
  Serial.println("[PROV] Timeout.");
}

// =====================
// Sensor
// =====================
THReading readBME280() {
  if (!bmeFound) return {0, 0, 0, false};
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0f;
  if (isnan(t) || isnan(h)) return {0, 0, 0, false};
  return {t, h, p, true};
}

// =====================
// JSON payload
// =====================
void buildJsonPayload(String &out, const THReading& amb) {
  out = "";
  DynamicJsonDocument doc(256);
  doc["device_id"] = g_device_id;

  JsonObject a = doc.createNestedObject("amb");
  a["ok"] = amb.ok;
  if (amb.ok) {
    a["temp_c"]       = amb.temp_c;
    a["hum_pct"]      = amb.hum_pct;
    a["pressure_hpa"] = amb.pressure_hpa;
  }

  JsonObject r = doc.createNestedObject("relay");
  r["on"] = g_relay_on;

  serializeJson(doc, out);
}

// =====================
// Procesar cualquier paquete LoRa recibido en la ventana de comandos
// {"target","type","params"}
// =====================
void processPacket(const String& raw) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, raw)) return;

  const char* type   = doc["type"];
  const char* target = doc["target"];
  if (type && target) {
    processCommand(raw);
  }
}

// =====================
// Procesar comandos de control
// =====================
void processCommand(const String& rawCmd) {
  StaticJsonDocument<256> cmdDoc;
  if (deserializeJson(cmdDoc, rawCmd)) return;

  const char* target = cmdDoc["target"];
  if (!target || g_device_id != target) return;

  const char* cmdType = cmdDoc["type"];
  if (!cmdType) return;

  Preferences prefs;
  if (strcmp(cmdType, "set_sleep") == 0) {
    int min = cmdDoc["params"]["minutes"] | g_sleep_min;
    min = constrain(min, 1, 1440);
    prefs.begin("agro", false);
    prefs.putInt("sleep_min", min);
    prefs.end();
    g_sleep_min = min;
    Serial.printf("[CMD] sleep -> %d min\n", min);
  }
  else if (strcmp(cmdType, "set_device_id") == 0) {
    const char* newId = cmdDoc["params"]["device_id"];
    if (newId && strlen(newId) > 0) {
      prefs.begin("agro", false);
      prefs.putString("device_id", newId);
      prefs.end();
      Serial.printf("[CMD] device_id -> %s\n", newId);
      delay(500); ESP.restart();
    }
  }
  else if (strcmp(cmdType, "restart") == 0) {
    Serial.println("[CMD] Reiniciando...");
    delay(500); ESP.restart();
  }
  else if (strcmp(cmdType, "set_relay") == 0) {
    bool on = cmdDoc["params"]["state"] | false;
    applyRelay(on);
    prefs.begin("agro", false);
    prefs.putBool("relay_on", on);
    prefs.end();
    Serial.printf("[CMD] relay -> %s\n", on ? "ON" : "OFF");
  }
  else {
    Serial.printf("[CMD] Tipo desconocido: %s\n", cmdType);
  }
}

// =====================
// Fin de ciclo — TX cada ~10s para pruebas de enlace LoRa
// (igual que el emisor DOIT: antes de desplegar a batería, cambiar
// a esp_sleep_enable_timer_wakeup() + esp_deep_sleep_start())
// =====================
void finishCycle() {
  delay(10000);
}

// =====================
// Setup
// =====================
void setup() {
  Serial.begin(115200);
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) delay(10);
  delay(300);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("   EMISOR XIAO AgroESP32  |  LoRa (RFM95W) + BME280");
  Serial.println("============================================================");

  loadConfig();

  pinMode(RELAY_PIN, OUTPUT);
  applyRelay(g_relay_on);   // restaura el último estado conocido (NVS)
  Serial.printf("[RELAY] Pin %d -> %s\n", RELAY_PIN, g_relay_on ? "ON" : "OFF");

  Serial.println();
  Serial.println("[ SENSOR ]");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  bmeFound = bme.begin(BME280_I2C_ADDR, &Wire);
  Serial.println(bmeFound ? "  BME280  : OK" : "  BME280  : NO encontrado");

  Serial.println();
  Serial.println("[ LoRa ]");
  if (!initLoRa()) {
    Serial.println("  ERROR: RFM95W no detectado. Revisa conexiones (CS, RST, SPI, alimentacion).");
    finishCycle();
  }
  Serial.printf("  Frecuencia : %.1f MHz\n", LORA_FREQ_MHZ);
  Serial.printf("  TX Power   : %d dBm\n", LORA_TX_DBM);
}

// =====================
// Loop: una vez, TX + ventana de comandos LoRa, y duerme
// =====================
void loop() {
  THReading amb = readBME280();

  String payload;
  buildJsonPayload(payload, amb);

  Serial.println();
  Serial.println("[ TRANSMISION ]");
  if (amb.ok) Serial.printf("  Ambiente : %.1f C  %.1f%% HR  %.1f hPa\n", amb.temp_c, amb.hum_pct, amb.pressure_hpa);
  else        Serial.println("  Ambiente : sin datos");
  Serial.printf("  Relay    : %s\n", g_relay_on ? "ON" : "OFF");
  Serial.printf("  Payload  : %d bytes\n", payload.length());
  Serial.printf("  JSON     : %s\n", payload.c_str());

  Serial.println("\n[ LoRa TX ]");
  int txState = radio.transmit((uint8_t*)payload.c_str(), payload.length());
  if (txState == RADIOLIB_ERR_NONE) {
    Serial.println("  Enviado.");
  } else {
    Serial.printf("  ERROR TX: %d\n", txState);
  }

  // =====================
  // Ventana de comandos: escucha LoRa por si el receptor
  // relay-ea un comando pendiente para este device_id.
  // =====================
  Serial.printf("\n[ CMD WINDOW ] %d ms\n", CMD_WINDOW_MS);
  unsigned long cmdT0 = millis();
  while (millis() - cmdT0 < CMD_WINDOW_MS) {
    unsigned long elapsed = millis() - cmdT0;
    unsigned long remaining = (elapsed < CMD_WINDOW_MS) ? (CMD_WINDOW_MS - elapsed) : 0;
    uint8_t buf[MAX_LORA + 1];
    int rxState = radio.receive(buf, 0, remaining);
    if (rxState == RADIOLIB_ERR_NONE) {
      size_t len = radio.getPacketLength();
      if (len > (size_t)MAX_LORA) len = MAX_LORA;
      buf[len] = '\0';
      String raw = String((char*)buf);
      Serial.printf("  CMD RX: %s (RSSI %.0f)\n", raw.c_str(), radio.getRSSI());
      processPacket(raw);
    }
  }

  finishCycle();
}
