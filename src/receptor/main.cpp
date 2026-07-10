// =====================================================
// RECEPTOR (GATEWAY) — LoRa (RFM95W) <- EMISOR -> WiFi -> API
// Recibe JSON por LoRa, lo reenvía a la API REST.
// Relay de comandos: tras cada POST exitoso consulta
//   GET /api/relay/?token=...&emitter=<device_id>
//   y retransmite comandos pendientes por LoRa.
//
// Config (prioridad):
//   1. NVS (Preferences)   — provisioning por serial
//   2. secrets.h / board_config.h — generados por la plataforma
//   Si ninguno: modo provisioning (espera JSON por serial 2 min)
// =====================================================
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_wifi.h>
#include <Preferences.h>

#include "board_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// =====================
// Config global
// =====================
String g_device_id  = "";
String g_wifi_ssid  = "";
String g_wifi_pass  = "";
String g_api_url    = "";
String g_api_token  = "";

// =====================
// Estado WiFi
// =====================
unsigned long lastWifiRetry    = 0;
unsigned long lastHeartbeat    = 0;
bool          wifiWasConnected = false;

// =====================
// LoRa (RadioLib — ver nota en src/emisor/main.cpp sobre por
// que se migro desde RadioHead)
// =====================
Module loraModule(LORA_CS_PIN, LORA_DIO0_PIN, LORA_RST_PIN, RADIOLIB_NC, SPI, SPISettings(500000, MSBFIRST, SPI_MODE0));
SX1276 radio(&loraModule);
const int MAX_LORA = 250;

volatile bool loraRxFlag = false;
void IRAM_ATTR onLoraRx() {
  loraRxFlag = true;
}

bool initLoRa() {
  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);

  int state = radio.begin(LORA_FREQ_MHZ, 125.0, 7, 5, LORA_SYNC_WORD, LORA_TX_DBM, 8, 0);
  if (state != RADIOLIB_ERR_NONE) return false;

  radio.setPacketReceivedAction(onLoraRx);
  radio.startReceive();
  return true;
}

// =====================
// Prototipos
// =====================
void loadConfig();
void enterProvisioningMode();
void connectWiFi();
bool postToAPI(const String& payload);
bool checkAndRelayCommands(const String& deviceId);
void ackCommand(int cmdId);

// =====================
// Configuración
// =====================
void loadConfig() {
  Preferences prefs;
  prefs.begin("agro", true);
  String nvsId = prefs.getString("device_id", "");
  if (nvsId.length() > 0) {
    g_device_id  = nvsId;
    g_wifi_ssid  = prefs.getString("wifi_ssid", "");
    g_wifi_pass  = prefs.getString("wifi_pass", "");
    g_api_url    = prefs.getString("api_url",   "");
    g_api_token  = prefs.getString("api_token", "");
    prefs.end();
    Serial.println("[CONFIG] NVS OK");
    return;
  }
  prefs.end();

#ifdef WIFI_SSID_SECRET
  g_device_id = "receptor";
  g_wifi_ssid  = WIFI_SSID_SECRET;
  g_wifi_pass  = WIFI_PASS_SECRET;
  g_api_url    = API_URL_SECRET;
  g_api_token  = API_TOKEN_SECRET;
  Serial.println("[CONFIG] secrets.h OK");
  return;
#endif

  enterProvisioningMode();
}

void enterProvisioningMode() {
  Serial.println("[PROV] Sin configuracion. Modo provisioning activo.");
  String incoming = "";
  unsigned long deadline = millis() + 120000UL;
  unsigned long lastReady = 0;
  while (millis() < deadline) {
    if (millis() - lastReady >= 1000) { Serial.println("PROV_READY"); lastReady = millis(); }
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        incoming.trim();
        if (incoming.length() > 10) {
          StaticJsonDocument<512> cfg;
          if (!deserializeJson(cfg, incoming)) {
            const char* id    = cfg["device_id"];
            const char* ssid  = cfg["wifi_ssid"];
            const char* pass  = cfg["wifi_pass"];
            const char* url   = cfg["api_url"];
            const char* token = cfg["api_token"];
            if (ssid && token) {
              Preferences prefs; prefs.begin("agro", false);
              if (id && strlen(id) > 0) prefs.putString("device_id", id);
              prefs.putString("wifi_ssid", ssid);
              prefs.putString("wifi_pass", pass ? pass : "");
              prefs.putString("api_url",   url  ? url  : "");
              prefs.putString("api_token", token);
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
// WiFi
// =====================
void connectWiFi() {
  Serial.printf("[WiFi] Conectando a %s...\n", g_wifi_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_wifi_ssid.c_str(), g_wifi_pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifiWasConnected = true;
    Serial.printf("[WiFi] OK  IP=%s  MAC=%s\n",
      WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
  } else {
    Serial.println("[WiFi] FALLO — se reintentará");
  }
}

// =====================
// POST a API
// =====================
bool postToAPI(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.printf("  POST %s\n", g_api_url.c_str());

  for (int attempt = 1; attempt <= HTTP_POST_RETRIES; attempt++) {
    HTTPClient http;
    http.begin(g_api_url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + g_api_token);
    http.setTimeout(HTTP_POST_TIMEOUT_MS);

    int code = http.POST(payload);
    if (code == 200 || code == 201) {
      Serial.printf("  HTTP %d  OK\n", code);
      http.end();
      return true;
    }
    Serial.printf("  HTTP %d (intento %d/%d)\n", code, attempt, HTTP_POST_RETRIES);
    http.end();
    if (attempt < HTTP_POST_RETRIES) delay(HTTP_RETRY_DELAY_MS);
  }
  return false;
}

// =====================
// ACK de comando al backend
// =====================
void ackCommand(int cmdId) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = g_api_url;
  url.replace("telemetry/", "commands/");
  url += String(cmdId) + "/ack/?token=" + g_api_token;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(4000);
  int code = http.POST("");
  Serial.printf("  [ACK-CMD] cmd %d → HTTP %d\n", cmdId, code);
  http.end();
}

// =====================
// Relay de comandos al emisor
// =====================
bool checkAndRelayCommands(const String& deviceId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // GET /api/relay/?token=<receptor_token>&emitter=<emisor_device_id>
  String url = g_api_url;
  url.replace("telemetry/", "relay/");
  url += "?token=" + g_api_token + "&emitter=" + deviceId;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(HTTP_RELAY_TIMEOUT_MS);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("  [RELAY] HTTP %d (sin comandos o error)\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body)) {
    Serial.println("  [RELAY] JSON inválido");
    return false;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.size() == 0) {
    Serial.println("  [RELAY] Sin comandos pendientes");
    return false;
  }

  Serial.printf("  [RELAY] %d comando(s) para %s\n", cmds.size(), deviceId.c_str());

  for (JsonObject cmd : cmds) {
    StaticJsonDocument<256> loraCmd;
    loraCmd["target"]  = deviceId;
    loraCmd["cmd_id"]  = cmd["cmd_id"].as<int>();
    loraCmd["type"]    = cmd["type"].as<const char*>();
    loraCmd["params"]  = cmd["params"].as<JsonObject>();

    String cmdStr;
    serializeJson(loraCmd, cmdStr);
    Serial.printf("  [RELAY] TX: %s\n", cmdStr.c_str());

    int txState = radio.transmit((uint8_t*)cmdStr.c_str(), cmdStr.length());
    radio.startReceive();
    if (txState == RADIOLIB_ERR_NONE) {
      Serial.println("  [RELAY] Enviado");
    } else {
      Serial.printf("  [RELAY] ERROR TX: %d\n", txState);
    }
    ackCommand(cmd["cmd_id"].as<int>());
    delay(100);
  }
  return true;
}

// =====================
// Setup
// =====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("   RECEPTOR AgroESP32  |  LoRa (RFM95W) + WiFi");
  Serial.println("============================================================");

  loadConfig();
  connectWiFi();

  if (!initLoRa()) {
    Serial.println("[LoRa] ERROR: RFM95W no detectado — reiniciando...");
    delay(3000); ESP.restart();
  }

  Serial.printf("[LoRa] OK  %.1f MHz  TX %d dBm — esperando datos del emisor...\n",
    LORA_FREQ_MHZ, LORA_TX_DBM);
  Serial.println();
}

// =====================
// Loop
// =====================
void loop() {
  // --- Reconexión WiFi ---
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = millis();
      connectWiFi();
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat > 15000) {
    lastHeartbeat = millis();
    Serial.printf("[LISTEN] WiFi:%s  uptime:%.0fs\n",
      WiFi.status() == WL_CONNECTED ? "OK" : "NO", millis() / 1000.0);
  }

  if (!loraRxFlag) return;
  loraRxFlag = false;

  size_t len = radio.getPacketLength();
  if (len > (size_t)MAX_LORA) len = MAX_LORA;
  uint8_t buf[MAX_LORA + 1];
  int rxState = radio.readData(buf, len);
  int rssi = (int)radio.getRSSI();

  if (rxState != RADIOLIB_ERR_NONE) {
    Serial.printf("[RX LoRa] ERROR: %d\n", rxState);
    return;
  }
  buf[len] = '\0';

  Serial.println();
  Serial.println("============================================================");
  Serial.printf("[RX LoRa] RSSI: %d\n", rssi);
  Serial.printf("  Payload: %s\n", (char*)buf);

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, (char*)buf)) {
    Serial.println("  ERROR: JSON inválido");
    return;
  }

  const char* deviceId = doc["device_id"];
  if (!deviceId) {
    Serial.println("  ERROR: falta device_id");
    return;
  }
  String devId = String(deviceId);

  doc["rssi"] = rssi;

  String payload;
  serializeJson(doc, payload);

  bool posted = postToAPI(payload);
  if (posted) {
    checkAndRelayCommands(devId);
  }

  Serial.println("============================================================");
  Serial.println();
}
