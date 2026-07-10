// =====================================================
// EMISOR (NODO SENSOR) -> LoRa (RFM95W) -> RECEPTOR
// SHTC3 + DS18B20 + GPS + batería -> JSON -> LoRa
//
// El sensor ambiente físico es la placa GY-39 (HiLetgo,
// LTR308ALS+SPL06-007+SHTC3), pero solo se lee el SHTC3 que
// trae integrado — expone su dirección I2C estándar (0x70) en
// el mismo bus. El SPL06-007 (presión) no es compatible con el
// driver BME280 (registros distintos) y no está implementado.
//
// Tras cada TX abre una ventana de escucha (CMD_WINDOW_MS)
// por si el receptor relay-ea un comando pendiente para
// este device_id.
// =====================================================
#include <Wire.h>
#include <Adafruit_SHTC3.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <TinyGPSPlus.h>
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
String g_device_id    = "";
int    g_sleep_min    = DEFAULT_SLEEP_MIN;

bool g_en_SHTC3   = SENSOR_SHTC3_DEFAULT;
bool g_en_DS18B20 = SENSOR_DS18B20_DEFAULT;
bool g_en_GPS     = SENSOR_GPS_DEFAULT;

// =====================
// RTC memory — sobrevive deep sleep
// =====================
RTC_DATA_ATTR double   rtc_lat       = 0.0;
RTC_DATA_ATTR double   rtc_lon       = 0.0;
RTC_DATA_ATTR float    rtc_alt_m     = 0.0;
RTC_DATA_ATTR float    rtc_vel_kmh   = 0.0;
RTC_DATA_ATTR float    rtc_hdop      = 0.0;
RTC_DATA_ATTR uint32_t rtc_sats      = 0;
RTC_DATA_ATTR bool     rtc_gps_valid = false;

// =====================
// Tipos
// =====================
struct THReading  { float temp_c; float hum_pct; float pressure_hpa; bool has_pressure; bool ok; };
struct DSReading  { float temp_c; bool ok; };
struct GpsReading {
  bool valid;
  bool cached;
  double lat, lon;
  float alt_m, vel_kmh, hdop;
  uint32_t sats;
};

// =====================
// Hardware
// =====================
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
Adafruit_SHTC3 shtc3;
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds(&oneWire);
DeviceAddress dsAddr;
bool dsFound = false;

// =====================
// LoRa (RadioLib — RadioHead hacia SPI dentro del ISR de DIO0
// causaba reinicios durante rf95.send(); RadioLib resuelve TX/RX
// bloqueante haciendo polling del pin IRQ, sin SPI en interrupcion)
// =====================
Module loraModule(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, RADIOLIB_NC, SPI, SPISettings(500000, MSBFIRST, SPI_MODE0));
SX1276 radio(&loraModule);
const int MAX_LORA = 250;

bool initLoRa() {
  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);

  uint8_t sf = (LORA_SF_DEFAULT > 0) ? LORA_SF_DEFAULT : 7; // Sf128 = SF7
  int state = radio.begin(LORA_FREQ_MHZ, 125.0, sf, 5, LORA_SYNC_WORD, LORA_TX_DBM, 8, 0);
  return state == RADIOLIB_ERR_NONE;
}

// =====================
// Prototipos
// =====================
void readGPS();
void setupBatteryAdc();
float readVbat();
int vbatToPercent(float v);
THReading readSHTC3();
DSReading readDS18B20();
void buildJsonPayload(String &out, const THReading&, const DSReading&,
                      const GpsReading&, float vbat, int batPct, int gpsLevel);
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
  if (nvsId.length() > 0) {
    g_device_id      = nvsId;
    g_sleep_min      = prefs.getInt("sleep_min",   DEFAULT_SLEEP_MIN);
    g_en_SHTC3       = prefs.getBool("en_shtc3",   SENSOR_SHTC3_DEFAULT);
    g_en_DS18B20     = prefs.getBool("en_ds18b20", SENSOR_DS18B20_DEFAULT);
    g_en_GPS         = prefs.getBool("en_gps",     SENSOR_GPS_DEFAULT);
    prefs.end();
    Serial.println("[CONFIG] NVS OK");
    return;
  }
  prefs.end();

#ifdef DEVICE_ID_SECRET
  g_device_id      = DEVICE_ID_SECRET;
  g_sleep_min      = SLEEP_MINUTES;
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
// Batería ADC
// =====================
void setupBatteryAdc() { analogSetAttenuation(ADC_11db); }

float readVbat() {
  long sum = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) { sum += analogRead(BAT_ADC_PIN); delay(1); }
  float adc  = (float)(sum / BAT_SAMPLES);
  float vpin = (adc / BAT_ADC_MAX) * BAT_VREF;
  return vpin * BAT_DIV_RATIO;
}

int vbatToPercent(float v) {
  if (v >= BAT_V_MAX) return 100;
  if (v <= BAT_V_MIN) return 0;
  return (int)(100.0f * (v - BAT_V_MIN) / (BAT_V_MAX - BAT_V_MIN));
}

// =====================
// Sensores
// =====================
THReading readSHTC3() {
  if (!g_en_SHTC3) return {0, 0, 0, false, false};
  sensors_event_t h, t;
  shtc3.getEvent(&h, &t);
  if (isnan(t.temperature) || isnan(h.relative_humidity)) return {0, 0, 0, false, false};
  return {t.temperature, h.relative_humidity, 0, false, true};
}

DSReading readDS18B20() {
  if (!g_en_DS18B20 || !dsFound) return {0, false};
  ds.requestTemperatures();
  float temp = ds.getTempC(dsAddr);
  if (temp == DEVICE_DISCONNECTED_C) return {0, false};
  return {temp, true};
}

void readGPS() {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
}

// =====================
// JSON payload
// =====================
void buildJsonPayload(String &out, const THReading& amb, const DSReading& probe,
                      const GpsReading& gpsData, float vbat, int batPct, int gpsLevel) {
  out = "";
  DynamicJsonDocument doc(512);
  doc["device_id"] = g_device_id;

  if (g_en_SHTC3) {
    JsonObject a = doc.createNestedObject("amb");
    a["ok"] = amb.ok;
    if (amb.ok) {
      a["temp_c"] = amb.temp_c;
      a["hum_pct"] = amb.hum_pct;
      if (amb.has_pressure) a["pressure_hpa"] = amb.pressure_hpa;
    }
  }
  if (g_en_DS18B20) {
    JsonObject p = doc.createNestedObject("probe");
    p["ok"] = probe.ok;
    if (probe.ok) p["temp_c"] = probe.temp_c;
  }
  {
    JsonObject b = doc.createNestedObject("bat");
    b["v"]   = vbat;
    b["pct"] = batPct;
  }
  if (g_en_GPS && gpsData.valid && gpsLevel >= 0) {
    JsonObject g = doc.createNestedObject("gps");
    g["lat"] = gpsData.lat;
    g["lon"] = gpsData.lon;
    if (gpsLevel >= 1) {
      g["alt_m"]   = gpsData.alt_m;
      g["vel_kmh"] = gpsData.vel_kmh;
      g["hdop"]    = gpsData.hdop;
      g["sats"]    = gpsData.sats;
    }
    if (gpsData.cached) g["cached"] = true;
  }
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

  const char* target  = cmdDoc["target"];
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
  else if (strcmp(cmdType, "enable_sensor") == 0) {
    const char* sensor = cmdDoc["params"]["sensor"];
    bool en = cmdDoc["params"]["enable"] | true;
    if (sensor) {
      prefs.begin("agro", false);
      if (strcmp(sensor, "shtc3")   == 0) { g_en_SHTC3   = en; prefs.putBool("en_shtc3",   en); }
      if (strcmp(sensor, "ds18b20") == 0) { g_en_DS18B20  = en; prefs.putBool("en_ds18b20", en); }
      if (strcmp(sensor, "gps")     == 0) { g_en_GPS      = en; prefs.putBool("en_gps",     en); }
      prefs.end();
      Serial.printf("[CMD] sensor %s -> %s\n", sensor, en ? "on" : "off");
    }
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
  else {
    Serial.printf("[CMD] Tipo desconocido: %s\n", cmdType);
  }
}

// =====================
// Fin de ciclo — TX cada ~10s para pruebas de enlace LoRa
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
  Serial.println("   EMISOR AgroESP32  |  LoRa (RFM95W)");
  Serial.println("============================================================");

  loadConfig();
  setupBatteryAdc();

  Serial.println();
  Serial.println("[ SENSORES ]");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  if (g_en_SHTC3) {
    Serial.println(shtc3.begin() ? "  SHTC3   : OK" : "  SHTC3   : NO encontrado");
  }
  if (g_en_DS18B20) {
    ds.begin(); ds.setResolution(DS18B20_RESOLUTION);
    dsFound = ds.getAddress(dsAddr, 0);
    Serial.println(dsFound ? "  DS18B20 : OK" : "  DS18B20 : NO encontrado");
  }
  if (g_en_GPS) {
    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  }

  Serial.println();
  Serial.println("[ LoRa ]");
  if (!initLoRa()) {
    Serial.println("  ERROR: RFM95W no detectado. Revisa conexiones (CS, RST, SPI, alimentacion).");
    finishCycle();
  }
  Serial.printf("  Frecuencia : %.1f MHz\n", LORA_FREQ_MHZ);
  Serial.printf("  TX Power   : %d dBm\n", LORA_TX_DBM);

  if (g_en_GPS) {
    Serial.println();
    Serial.println("[ GPS ]");
    unsigned long gpsTimeout = rtc_gps_valid
      ? (unsigned long)GPS_TIMEOUT_WARM_S * 1000UL
      : (unsigned long)GPS_TIMEOUT_COLD_S * 1000UL;
    if (rtc_gps_valid)
      Serial.printf("  Cache: lat %.5f lon %.5f\n", rtc_lat, rtc_lon);
    else
      Serial.println("  Cold start");
    Serial.printf("  Timeout: %lu s\n", gpsTimeout / 1000);

    unsigned long t0 = millis(), lastProgress = 0;
    bool fixFound = false;
    while (millis() - t0 < gpsTimeout) {
      readGPS();
      if (gps.location.isValid() && gps.location.age() < 2000 && gps.satellites.value() >= GPS_MIN_SATS) {
        rtc_lat = gps.location.lat(); rtc_lon = gps.location.lng();
        rtc_alt_m = (float)gps.altitude.meters(); rtc_vel_kmh = (float)gps.speed.kmph();
        rtc_hdop = (float)gps.hdop.hdop(); rtc_sats = gps.satellites.value();
        rtc_gps_valid = true; fixFound = true;
        break;
      }
      if (millis() - lastProgress >= 5000) {
        lastProgress = millis();
        Serial.printf("  %2lus | sats: %u | valido: %s\n",
          (millis() - t0) / 1000, gps.satellites.value(),
          gps.location.isValid() ? "si" : "no");
      }
      delay(20);
    }
    if (fixFound)
      Serial.printf("  FIX OK: %.6f / %.6f  sats=%u\n", rtc_lat, rtc_lon, rtc_sats);
    else if (rtc_gps_valid)
      Serial.println("  Sin fix nuevo — usando cache RTC");
    else
      Serial.println("  SIN DATOS GPS");
  }
}

// =====================
// Loop: una vez, TX + ventana de comandos LoRa, y duerme
// =====================
void loop() {
  if (g_en_GPS) readGPS();

  THReading amb   = readSHTC3();
  DSReading probe = readDS18B20();
  float vbat   = readVbat();
  int   batPct = vbatToPercent(vbat);

  GpsReading gpsData = {false, false, 0, 0, 0, 0, 0, 0};
  if (g_en_GPS) {
    bool freshFix = gps.location.isValid() && gps.location.age() < 3000
                    && gps.satellites.value() >= GPS_MIN_SATS;
    if (freshFix) {
      gpsData = {true, false, gps.location.lat(), gps.location.lng(),
                 (float)gps.altitude.meters(), (float)gps.speed.kmph(),
                 (float)gps.hdop.hdop(), gps.satellites.value()};
    } else if (rtc_gps_valid) {
      gpsData = {true, true, rtc_lat, rtc_lon, rtc_alt_m, rtc_vel_kmh, rtc_hdop, rtc_sats};
    }
  }

  String payload;
  buildJsonPayload(payload, amb, probe, gpsData, vbat, batPct, 2);
  if (payload.length() > MAX_LORA) buildJsonPayload(payload, amb, probe, gpsData, vbat, batPct, 1);
  if (payload.length() > MAX_LORA) buildJsonPayload(payload, amb, probe, gpsData, vbat, batPct, 0);
  if (payload.length() > MAX_LORA) buildJsonPayload(payload, amb, probe, gpsData, vbat, batPct, -1);

  Serial.println();
  Serial.println("[ TRANSMISION ]");
  Serial.printf("  Bateria  : %.2f V  (%d%%)\n", vbat, batPct);
  if (amb.ok)        Serial.printf("  Ambiente : %.1f C  %.1f%% HR\n", amb.temp_c, amb.hum_pct);
  if (probe.ok)      Serial.printf("  Suelo    : %.1f C\n", probe.temp_c);
  if (gpsData.valid) Serial.printf("  GPS      : %.6f / %.6f  sats=%u%s\n",
    gpsData.lat, gpsData.lon, gpsData.sats, gpsData.cached ? " [cache]" : "");
  else               Serial.println("  GPS      : sin datos");
  Serial.printf("  Payload  : %d bytes\n", payload.length());
  Serial.printf("  JSON     : %s\n", payload.c_str());

  if (payload.length() > MAX_LORA) {
    Serial.println("  ERROR    : payload demasiado grande");
    finishCycle();
  }

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
