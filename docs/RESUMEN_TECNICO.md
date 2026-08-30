# AgroESP32 — Resumen del proyecto

> Última actualización: 2026-07-09. Este documento refleja el estado real del código a esta fecha. Incluye la reescritura completa de esa fecha (RadioLib, GY-39→SHTC3, sin deep sleep) más la incorporación, más tarde el mismo día, de un segundo tipo de nodo emisor sobre Seeed XIAO ESP32-S3 (ver §3.7 y §4.2).

## 1. Qué es este proyecto (explicación no técnica)

AgroESP32 es un sistema para **monitorear condiciones de un campo o cultivo a distancia**, sin necesidad de que una persona esté físicamente presente para tomar los datos.

La idea: un **dispositivo sensor** ("emisor") se coloca en el punto del campo a monitorear y mide:

- Temperatura y humedad del ambiente.
- Temperatura del suelo (sonda enterrada).
- Ubicación GPS.
- Nivel de batería.

El emisor transmite esas mediciones por radio de largo alcance a otro dispositivo (el "receptor" o "gateway", con WiFi), que sube los datos a una **plataforma web** (API en Railway) donde quedan disponibles para ver en un panel, graficar y generar alertas.

También permite el camino inverso: desde la plataforma se pueden enviar **órdenes** al emisor (cambiar el intervalo de envío, activar/desactivar un sensor, reiniciar, cambiar su ID), que el receptor reenvía por radio.

## 2. Arquitectura general

```
[ Emisor DOIT (multi-sensor) ]  --\
                                     radio LoRa 915MHz  -->  [ Receptor (gateway) ]  --WiFi/HTTPS-->  [ API en Railway ]
[ Emisor XIAO (BME280 solo) ]   --/                            ESP32 DOIT, WiFi                        Django + Next.js
```

**Dos tipos de nodo emisor, un solo receptor.** El receptor sigue siendo únicamente **ESP32 DOIT DevKit V1**. Los emisores ahora son de dos tipos:

- **Emisor DOIT** (`esp32doit-devkit-v1`, env `emisor`): SHTC3 + DS18B20 + GPS + batería. Ver §3.
- **Emisor XIAO** (`seeed_xiao_esp32s3`, env `emisor_xiao`): solo **BME280** (temp/hum/presión) por I2C. Nodo agregado el 2026-07-09 para poder sumar más puntos de medición sin repetir el hardware DOIT completo. Ver §3.7 y §4.2.

Ambos emisores hablan **el mismo protocolo LoRa** (mismo perfil de radio, mismo formato de comando, mismo esquema JSON base con `device_id`) y le llegan al **mismo receptor sin que este necesite ningún cambio** — el receptor solo lee `device_id` del JSON entrante y reenvía el payload tal cual a la API; no le importa qué hardware lo generó.

*(Nota histórica: el 2026-07-09, más temprano el mismo día, se había evaluado y descartado por completo un soporte XIAO anterior — ver bitácora §7.1. El soporte actual es un desarrollo nuevo e independiente de aquel intento revertido.)*

## 3. Hardware

### 3.1 Módulo de radio — LoRa (RFM95W)

Módulo **RFM95W** por SPI, en ambos nodos (emisor y receptor), mismos pines:

| Señal | GPIO ESP32 DOIT |
|---|---|
| VIN | 3.3V (⚠️ no 5V — el módulo no lo tolera) |
| GND | GND común con el resto de los sensores |
| CS (NSS) | GPIO 25 |
| RST | GPIO 14 |
| DIO0 | GPIO 26 |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |

Parámetros de radio (`board_config.h`):

| Parámetro | Valor |
|---|---|
| Frecuencia | 915.0 MHz |
| Spreading Factor | SF7 (default), configurable (`LORA_SF_DEFAULT`) |
| Bandwidth | 125 kHz |
| Sync word | `0x12` (red privada; `0x34` sería LoRaWAN público) |
| Potencia TX emisor | 20 dBm |
| Potencia TX receptor | 13 dBm |
| Payload máx | 250 bytes (`MAX_LORA`) |

DIO1–DIO5 no se usan (solo DIO0, requerido por RadioLib para el pin de interrupción).

### 3.2 Sensor ambiental — placa "GY-39" (HiLetgo), pero solo se usa el SHTC3

El sensor ambiente instalado es una placa vendida como **"GY-39"** (HiLetgo, [ASIN B07QV5136P](https://www.amazon.com/dp/B07QV5136P)), que en realidad es un módulo **4-en-1**: LTR308ALS (luz) + **SPL06-007** (presión) + **SHTC3** (temperatura/humedad), con un microcontrolador propio que agrega los tres sensores vía un protocolo propietario (UART o I2C, configurable).

**Importante:** el firmware **no usa el protocolo propietario del módulo ni la presión (SPL06-007)**. Se probó tratar la placa como un BME280 (driver `Adafruit_BME280`) y nunca funcionó — el SPL06-007 no comparte registros con el BME280, así que `begin()` fallaba siempre en silencio. Lo que sí funciona, y es lo que usa el firmware, es leer el **chip SHTC3 directamente por su dirección I2C estándar (0x70)**, que la placa expone igual que si fuera un SHTC3 suelto:

| Señal | GPIO ESP32 DOIT |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 3.3V |
| GND | GND |

No hay lectura de presión atmosférica ni de luz en este proyecto (en el emisor DOIT — el emisor XIAO sí lee presión, ver §3.7). Si se quisiera agregar presión al DOIT en el futuro, hace falta un driver específico para el SPL06-007 (chip y protocolo distintos al BME280) — no reintentar con `Adafruit_BME280`.

### 3.3 DS18B20 (sonda de temperatura de suelo)

OneWire, un solo pin:

| Señal | GPIO ESP32 DOIT |
|---|---|
| Data | GPIO 4 (con resistencia pull-up a 3.3V, típico 4.7kΩ) |
| VCC | 3.3V |
| GND | GND |

Resolución configurable 9–12 bits (`DS18B20_RESOLUTION`, default 12).

### 3.4 GPS (vía TinyGPSPlus)

UART por software (`HardwareSerial` #1):

| Señal | GPIO ESP32 DOIT |
|---|---|
| RX (ESP32) ← TX (GPS) | GPIO 16 |
| TX (ESP32) → RX (GPS) | GPIO 17 |

9600 baudios. Arranque en frío: hasta 60s de espera de fix (`GPS_TIMEOUT_COLD_S`). Arranque en caliente (con caché en RTC memory de deep sleep): hasta 30s (`GPS_TIMEOUT_WARM_S`). Requiere mínimo 3 satélites (`GPS_MIN_SATS`) para considerar el fix válido.

### 3.5 Batería

Lectura por ADC con divisor resistivo:

| Señal | GPIO ESP32 DOIT |
|---|---|
| ADC | GPIO 34 |

Divisor 1:1 (R1=R2, `BAT_DIV_RATIO=2.0`). Curva lineal simple: 4.20V = 100%, 3.20V = 0% (`BAT_V_MAX`/`BAT_V_MIN`, pensado para Li-Ion; para LiFePO4 el máximo real sería ~3.65V — revisar si se cambia de química).

### 3.6 Receptor — hardware adicional

El receptor usa el WiFi integrado del ESP32 DOIT (no hardware extra más allá del RFM95W). No lleva sensores ambientales ni batería — se alimenta por USB/fuente fija.

### 3.7 Emisor XIAO — Seeed XIAO ESP32-S3 + RFM95W + BME280

Segundo tipo de nodo emisor, agregado el 2026-07-09. El XIAO ESP32-S3 tiene muchos menos GPIO que el DOIT (11 pines de propósito general, `D0`–`D10`, contra ~30 del DOIT), así que este nodo lleva **un solo sensor: BME280** (temperatura, humedad y presión atmosférica reales — a diferencia de la placa GY-39 del §3.2, este es un BME280 genuino y el driver `Adafruit_BME280` sí funciona). Sin DS18B20, sin GPS, sin batería monitoreada por ahora.

**RFM95W (módulo externo por SPI) — mismo perfil de radio que el DOIT (915 MHz, SF7, sync word `0x12`):**

| Señal | Pin XIAO |
|---|---|
| VIN | 3.3V (⚠️ no 5V) |
| GND | GND |
| NSS (CS) | D3 |
| RST | D1 |
| DIO0 | D2 |
| SCK | D8 |
| MISO | D9 |
| MOSI | D10 |

**BME280 (I2C):**

| Señal | Pin XIAO |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | D4 |
| SCL | D5 |
| Dirección I2C | `0x76` (`BME280_I2C_ADDR` en `board_config.h` — algunos breakouts usan `0x77`) |

**Pines usados en total: `D1, D2, D3, D4, D5, D8, D9, D10` (8 de 11).**

**Pines libres: `D0`, `D6`, `D7`** — ver §9 para qué se puede hacer con ellos.

**Alimentación:** el XIAO ESP32-S3 trae gestión de carga integrada. Para operación a batería independiente (sin USB), se conecta una Li-Po/Li-ion de una celda (3.7V nominal) al conector JST 1.25mm de 2 pines marcado `BAT` en la parte de abajo de la placa — no requiere pines GPIO ni código adicional; con USB conectado carga la batería automáticamente, sin USB alimenta solo de batería.

**Payload JSON** (más chico que el del DOIT, sin `probe`/`gps`/`bat`):
```json
{
  "device_id": "e2",
  "amb": { "ok": true, "temp_c": 24.8, "hum_pct": 52.3, "pressure_hpa": 1013.2 }
}
```

## 4. Firmware — estructura del proyecto

Tres entornos en `platformio.ini`:

- **`env:emisor`** (`board = esp32doit-devkit-v1`) → compila `src/emisor/main.cpp`
- **`env:receptor`** (`board = esp32doit-devkit-v1`) → compila `src/receptor/main.cpp`
- **`env:emisor_xiao`** (`board = seeed_xiao_esp32s3`) → compila `src/emisor_xiao/main.cpp`

Librerías comunes: `ArduinoJson`, **`RadioLib`** (no RadioHead — ver nota histórica en §7). Además, `emisor` usa `Adafruit SHTC3`, `Adafruit Unified Sensor`, `OneWire`, `DallasTemperature`, `TinyGPSPlus`; `emisor_xiao` usa `Adafruit BME280 Library` + `Adafruit Unified Sensor`.

Cada carpeta `src/<nodo>/` tiene `board_config.h` (pines/parámetros de radio) y `secrets.h` (config específica del dispositivo — **no versionado**, copiar desde el `.example` correspondiente, ver §10).

### 4.1 Nodo emisor DOIT — ciclo de vida

1. **Arranque y carga de configuración** (`loadConfig`): NVS (`Preferences`) → si no hay, `secrets.h` (generado por la plataforma) → si tampoco, **modo provisioning** (espera hasta 90s un JSON por serial: `{"device_id":"...", "sleep_min": N}`).
2. **Inicialización de sensores** habilitados (SHTC3, DS18B20, GPS) y del módulo LoRa.
3. **Espera de GPS** (si está habilitado), con caché en `RTC_DATA_ATTR` que sobrevive deep sleep.
4. **Loop** (una sola pasada por ciclo):
   - Lee GPS, SHTC3, DS18B20, batería.
   - Arma el JSON del payload, recortando progresivamente el bloque GPS si no entra en 250 bytes (nivel 2: todo → nivel 1: sin alt/vel/hdop → nivel 0: solo lat/lon → nivel -1: sin GPS).
   - Transmite por LoRa (`radio.transmit`).
   - Abre una **ventana de comandos** de `CMD_WINDOW_MS` (7000ms) escuchando por si el receptor reenvía una orden pendiente.
   - Llama a `finishCycle()`.

**⚠️ Nota importante:** `finishCycle()` actualmente es `delay(10000)` — un loop continuo que transmite cada ~10s, **no** `esp_deep_sleep_start()`. Quedó así de una sesión de test de enlace LoRa (2026-07-09) para poder iterar rápido sin esperar ciclos de wake-up. El diseño original (y lo que tiene sentido para un nodo a batería en campo) es deep sleep de `DEFAULT_SLEEP_MIN` minutos (10 por defecto, configurable por NVS/comando remoto). **Antes de desplegar a campo con batería, cambiar `finishCycle()` para que llame a `esp_sleep_enable_timer_wakeup()` + `esp_deep_sleep_start()`** — si no, el consumo va a ser el de un nodo despierto permanentemente, no el de meses de autonomía.

### 4.2 Nodo emisor XIAO — ciclo de vida

Mismo esqueleto que el emisor DOIT (`loadConfig` NVS→secrets.h→provisioning, TX + ventana de comandos, `finishCycle`), simplificado a un solo sensor:

1. **Arranque y carga de configuración**: idéntico al DOIT (mismo namespace NVS `"agro"`, mismo formato de provisioning por serial).
2. **Inicialización**: solo BME280 (I2C) y RFM95W. Sin GPS, sin OneWire, sin ADC de batería — no hay código para ninguno de los tres en `src/emisor_xiao/main.cpp`.
3. **Loop** (una sola pasada por ciclo):
   - Lee BME280 (`readBME280()`); si `bme.begin()` falló al arrancar, transmite igual con `amb.ok:false` (sirve para probar el enlace de radio sin el sensor conectado).
   - Arma el JSON (`buildJsonPayload`) — sin lógica de recorte por tamaño como el DOIT, porque el payload siempre es chico (no lleva GPS).
   - Transmite por LoRa, abre la misma ventana de comandos de `CMD_WINDOW_MS`, y llama a `finishCycle()`.

**Comandos soportados:** `set_sleep`, `set_device_id`, `restart` (mismo formato que el DOIT, ver §4.5). **No** soporta `enable_sensor` — no tiene sentido con un solo sensor fijo.

**Mismo `finishCycle()` de prueba que el DOIT** (`delay(10000)`, no deep sleep) — aplica la misma advertencia del §4.1 antes de llevarlo a batería en campo.

### 4.3 Nodo receptor (gateway)

1. Carga de configuración (NVS → `secrets.h` → provisioning; además de `device_id`/`sleep_min`, incluye `wifi_ssid`, `wifi_pass`, `api_url`, `api_token`).
2. Conexión WiFi con reintentos automáticos (`WIFI_RETRY_MS`).
3. LoRa en modo recepción continua (interrupción por paquete vía `radio.setPacketReceivedAction`).
4. Al recibir un paquete: parsea el JSON, le agrega `rssi` (real, de `radio.getRSSI()`), y hace **POST** a la API (`postToAPI`, con reintentos `HTTP_POST_RETRIES`).
5. Si el POST fue exitoso, **`checkAndRelayCommands`**: hace `GET /api/relay/?token=<receptor_token>&emitter=<device_id_emisor>`, y si hay comandos pendientes los retransmite por LoRa y confirma cada uno con `POST /api/commands/<id>/ack/`.

### 4.4 Formato del payload JSON (emisor → receptor → API)

Emisor DOIT:
```json
{
  "device_id": "e1",
  "amb":   { "ok": true, "temp_c": 28.5, "hum_pct": 57.9 },
  "probe": { "ok": true, "temp_c": 26.7 },
  "bat":   { "v": 4.66, "pct": 100 },
  "gps":   { "lat": -34.6, "lon": -58.4, "alt_m": 25.0, "vel_kmh": 0.0, "hdop": 1.2, "sats": 7 },
  "rssi": -42
}
```

Emisor XIAO (ver §3.7) — mismo esquema base, subset de campos:
```json
{
  "device_id": "e2",
  "amb": { "ok": true, "temp_c": 24.8, "hum_pct": 52.3, "pressure_hpa": 1013.2 },
  "rssi": -38
}
```

`amb`, `probe` y `gps` solo aparecen si el sensor correspondiente está habilitado (o, en el XIAO, si existe en ese firmware); `rssi` lo agrega el receptor, no el emisor. El receptor no distingue entre tipos de emisor — reenvía cualquier JSON que traiga `device_id` válido.

### 4.5 Comandos remotos soportados

Enviados desde la plataforma → relay del receptor → LoRa → emisor. Formato del paquete de comando: `{"target":"<device_id>","type":"...","params":{...}}`.

| Comando | Params | Efecto |
|---|---|---|
| `set_sleep` | `{"minutes": N}` (1–1440) | Cambia el intervalo entre ciclos, persiste en NVS |
| `enable_sensor` | `{"sensor": "shtc3"\|"ds18b20"\|"gps", "enable": bool}` | Activa/desactiva un sensor, persiste en NVS |
| `set_device_id` | `{"device_id": "..."}` | Reasigna el ID del dispositivo y reinicia |
| `restart` | `{}` | Reinicio por software |

No hay comandos de LoRa (`set_lora_sf`/`set_lora_power`) ni de ESP-NOW en la versión actual — existieron en iteraciones previas y fueron retirados.

### 4.6 NVS (persistencia)

Namespace `"agro"` (`Preferences`), compartido por los tres firmwares. Claves usadas por el emisor DOIT: `device_id`, `sleep_min`, `en_shtc3`, `en_ds18b20`, `en_gps`. Por el emisor XIAO: `device_id`, `sleep_min` (sin flags de sensores — solo tiene el BME280, siempre activo). Por el receptor: `device_id`, `wifi_ssid`, `wifi_pass`, `api_url`, `api_token`.

## 5. Backend / plataforma web (agroplatform)

Repo aparte, **sí con git**: `C:\Users\diego\OneDrive\Documentos\agroplatform\` — GitHub `diegoPonton/AgroPlatfrom`, branch `main`. Railway hace auto-deploy de ese branch; `backend/Procfile` corre `python manage.py migrate --no-input` en la fase `release`, así que las migraciones se aplican solas en cada deploy.

**Stack:**
- Backend: Django 5 + Django REST Framework + Channels (WebSocket) + PostgreSQL + Redis, todo en Railway.
- Frontend: Next.js (App Router) + shadcn/ui + TanStack Query + Recharts.

**Apps Django:** `users` (auth JWT), `devices` (dispositivos, comandos, topología), `telemetry` (ingesta y relay), `firmware` (generación de `board_config.h`/`secrets.h` y compilación), `alerts` (reglas y eventos de alerta).

**Endpoints principales:**

```
POST /api/auth/register/        POST /api/auth/login/        POST /api/auth/refresh/     GET /api/auth/me/

GET/POST /api/devices/                    GET /api/devices/receptors/
GET/PATCH/DELETE /api/devices/<id>/       GET /api/devices/topology/
GET /api/devices/gps-map/                 GET /api/devices/<id>/secrets/
GET /api/devices/<id>/provision/          GET /api/devices/<id>/gateway-stats/
GET/POST /api/devices/<id>/commands/      POST /api/devices/<id>/rotate-token/
GET /api/commands/<cmd_id>/               POST /api/commands/<cmd_id>/ack/
GET /api/relay/?token=...&emitter=...     (usado por el receptor)

POST /api/telemetry/                      (usado por el receptor, Bearer = provisioning_token)
GET /api/devices/<id>/telemetry/          (historial)

GET /api/firmware/                        GET/POST /api/firmware/<id>/status/, /download/
GET/POST /api/devices/<id>/firmware-config/   POST /api/devices/<id>/build/
POST /api/devices/<id>/flash/             GET /api/devices/<id>/flash-logs/

GET/POST /api/devices/<id>/alerts/        GET/PATCH/DELETE /api/alerts/<id>/     GET /api/alerts/events/

WS /ws/devices/<device_id>/               (telemetría en tiempo real, Django Channels)
```

**Autenticación del receptor hacia la API:** el receptor manda `Authorization: Bearer <provisioning_token>` y el `device_id` del **emisor** en el body de `/api/telemetry/`. El backend registra qué receptor (`source_gateway`) recibió cada lectura.

**Modelos relevantes (`apps/devices/models.py`):**
- `Device`: `device_type` (`emisor`/`receptor`), `assigned_gateway` (FK self, emisor→receptor), `provisioning_token`, `config` (JSON con pines/credenciales), `is_online` (últimos 5 min). **No tiene** campo de variante de placa/hardware — un `Device` tipo `emisor` no distingue si es un DOIT o un XIAO; eso solo vive en qué firmware se le flasheó.
- `DeviceCommand`: `command_type` (ver §4.5), `status` (`pending`/`relayed`/`acked`/`failed`).
- `Sensor`: `sensor_type` ∈ `SHTC3`, `BME280`, `DS18B20`, `GPS`, `BAT` (`BME280` agregado 2026-07-09, migración `0011_bme280_sensor_type`, para el nodo emisor XIAO del §3.7/§4.2).

**Requisito para que el receptor no devuelva 404 en `/api/telemetry/`:** el `device_id` que manda el emisor (p. ej. `e2` del XIAO) tiene que existir como `Device` registrado, **en la misma `organization` que el receptor** — `ingest()` en `apps/telemetry/views.py` hace `Device.objects.get(device_id=device_id, organization=receptor.organization)` y devuelve 404 si no lo encuentra. No hay auto-registro: cada emisor nuevo (sea DOIT o XIAO) se da de alta a mano desde "Registrar nuevo nodo" en el dashboard, con el mismo `device_id` que tiene en su `secrets.h`/NVS, y asignado al receptor correspondiente.

**Dashboard del emisor XIAO:** la página de detalle de dispositivo (`frontend/.../devices/[id]/page.tsx`) lee el payload por path genérico (`amb.temp_c`, `amb.hum_pct`, `amb.pressure_hpa`, etc.), no por `Sensor.sensor_type` registrado — así que ya mostraba temp/humedad del XIAO sin cambios. Se agregó una tarjeta y gráfico de **Presión** (`amb.pressure_hpa`) el 2026-07-09 porque no existía visualización para ese campo (el DOIT nunca lo manda).

**Generación de firmware:** `apps/firmware/views.py` genera `board_config.h` y `secrets.h` por dispositivo a partir de `Device.config`, compila con PlatformIO (o Arduino CLI) en un directorio temporal, y arma el binario final (bootloader + partitions + app fusionados) para flashear por Web Serial API (`esptool-js`) desde el navegador — `/devices/<id>/flash`. **No soporta el emisor XIAO** — el generador de config y el flasheo web siguen asumiendo `esp32doit-devkit-v1`; el nodo XIAO se compila y flashea a mano por PlatformIO CLI (§6), fuera del flujo de la plataforma. Si se quiere flashear XIAO desde el navegador, es trabajo pendiente, no hecho.

**Para levantar en local:**
```
cd backend
.\venv\Scripts\activate
python manage.py migrate
daphne -b 0.0.0.0 -p 8000 config.asgi:application
```
```
cd frontend
npm run dev
```

## 6. Cómo compilar y flashear (PlatformIO CLI)

Parado en el repo del firmware:

```
pio run -e emisor -t upload --upload-port COMx
pio device monitor -e emisor -p COMx

pio run -e receptor -t upload --upload-port COMx
pio device monitor -e receptor -p COMx

pio run -e emisor_xiao -t upload --upload-port COMx
pio device monitor -e emisor_xiao -p COMx
```

Si PlatformIO no está en el PATH: `C:\Users\diego\.platformio\penv\Scripts\pio.exe`.

## 7. Decisiones y problemas resueltos (bitácora)

### 7.1 2026-07-09, primera mitad del día — reescritura y reversión de XIAO (histórico)

1. **Soporte XIAO revertido por completo.** Se había agregado una variante de emisor sobre Seeed XIAO ESP32-S3 (un solo sensor por nodo, pines remapeados por `#if BOARD_XIAO_ESP32S3`) tanto en el firmware como en agroplatform (`board_variant`, `env:emisor_xiao`, generador de config, wizard del frontend). Se revirtió todo — no quedó ningún rastro de XIAO en el código de ese momento.
2. **RadioHead vs RadioLib.** Al revertir XIAO se restauró por error la librería **RadioHead** (`RH_RF95`), que es la que el propio historial de commits de agroplatform documenta como rota para este proyecto: tras los ~60s de espera de GPS, el módulo SX1276 no volvía a STANDBY y el TX daba timeout siempre (`IRQ_FLAGS=0x00`, nunca `TxDone`) — por eso en su momento se había migrado a **RadioLib**. Se corrigió el mismo día volviendo a RadioLib, que es lo que usa el firmware actual.
3. **GY-39 ≠ BME280.** Se agregó soporte para un supuesto sensor "GY-39" usando el driver `Adafruit_BME280`, asumiendo que era una placa BME280. La placa real (HiLetgo, ver §3.2) usa un SPL06-007 para presión, no compatible con ese driver — nunca funcionó. Se sacó el driver BME280 por completo y se dejó el SHTC3 (que sí es un chip real de esa placa, y sí se lee bien por I2C directo) como único sensor ambiente.
4. **ESP-NOW fue un parche temporal** (desde 2026-06-18) mientras se reemplazaba un módulo RFM95W dañado — se restauró LoRa como transporte definitivo al llegar el módulo nuevo, el mismo 2026-07-09.

### 7.2 2026-07-09, segunda mitad del día — nuevo nodo emisor XIAO (BME280)

Desarrollo nuevo e independiente del intento revertido en §7.1 (no se restauró nada de ahí):

1. **Nuevo entorno `env:emisor_xiao`** en `platformio.ini` (`board = seeed_xiao_esp32s3`) + `src/emisor_xiao/{main.cpp, board_config.h, secrets.h}`, sin tocar `src/emisor/` ni `src/receptor/`. Mismo protocolo LoRa/JSON/comandos que el emisor DOIT, sensor único BME280 real (ver §3.7/§4.2).
2. **Pin de CS del RFM95W ajustado de `D0` a `D3`** durante el cableado real del usuario (el diseño inicial proponía `D0`; el cableado físico terminó en `D3`, así que se actualizó `board_config.h` para que coincida).
3. **Enlace LoRa probado end-to-end**: el nodo XIAO (`e2`) transmite y el receptor DOIT existente lo recibe **sin ningún cambio en `src/receptor/`** — confirma que el receptor es agnóstico al tipo de emisor (rutea todo por `device_id` del JSON).
4. **BME280 conectado y verificado** (`SDA=D4`, `SCL=D5`, dirección `0x76`) — pasa de `amb.ok:false` a lecturas reales de temp/hum/presión.
5. **Backend (`agroplatform`, repo aparte) actualizado y desplegado a Railway** (commit `7599a43`, push a `main`): se agregó `BME280` a `Sensor.SENSOR_TYPES` (migración `0011_bme280_sensor_type`), al selector de sensores del formulario "Registrar nuevo nodo", y una tarjeta/gráfico de **Presión** en el detalle de dispositivo. Detalle completo en §5.
6. **Causa del primer HTTP 404 identificada:** no era un bug — el emisor XIAO (`e2`) todavía no estaba registrado como `Device` en la plataforma. El endpoint de ingesta exige que todo `device_id` entrante ya exista, en la misma organización que el receptor. Se resolvió dando de alta el dispositivo desde el dashboard, no con un cambio de código.
7. **Pendiente, no hecho:** deep sleep real en `emisor_xiao` (sigue en `delay(10000)` de prueba, igual que el DOIT — ver advertencia en §4.1/§4.2); soporte del nodo XIAO en el generador de firmware/flasheo web de la plataforma (§5); monitoreo de batería en el XIAO (sin pin ADC cableado todavía — ver §9).
8. **Repositorio de firmware inicializado en git y publicado en GitHub** — hasta este punto este repo no tenía control de versiones. Ver §10.

## 8. Resumen técnico en una frase

Red de sensores agrícolas: dos tipos de nodo emisor — **ESP32 DOIT** (SHTC3 de una placa GY-39, DS18B20, GPS, batería) y **Seeed XIAO ESP32-S3** (BME280 real) — transmiten por **LoRa vía RadioLib** (RFM95W, 915 MHz, red privada) a un receptor/gateway ESP32 DOIT con WiFi, que publica los datos en una API REST (Django/Next.js sobre Railway, auto-deploy desde GitHub) y sirve de canal bidireccional para comandos de configuración remota.

## 9. Nodo XIAO — pines libres y qué se puede hacer con ellos

Ver tabla completa de pines usados en §3.7. De los 11 GPIO de propósito general del XIAO ESP32-S3 (`D0`–`D10`), quedan **3 libres: `D0`, `D6`, `D7`**.

| Pin libre | GPIO | Capacidad | Uso típico |
|---|---|---|---|
| `D0` | GPIO1 | Digital **y ADC** (`A0`) | Único pin libre que sirve para leer voltaje analógico — necesario para monitoreo de batería |
| `D6` | GPIO43 | Digital / UART TX | Solo digital — no es pin ADC |
| `D7` | GPIO44 | Digital / UART RX | Solo digital — no es pin ADC |

**Restricción clave: solo `D0` puede leer voltaje analógico.** `D6`/`D7` son GPIO digitales puros en el ESP32-S3 (no están en el rango de pines con ADC), así que **cualquier combinación que necesite más de un pin ADC no entra** — no hay margen para eso con los pines que quedan.

**Posibilidades concretas con lo que queda:**

1. **Batería monitoreada** (como el DOIT, §3.5): armar un divisor resistivo (2 resistencias iguales, ej. 100kΩ) desde `BAT+`/el conector JST hacia `D0`, igual patrón que `BAT_ADC_PIN` del DOIT. Es la opción más directa y probablemente la más útil para un nodo a batería en campo — hoy el XIAO no reporta batería.
2. **GPS** (como el DOIT, §3.4): TinyGPSPlus por UART en `D6` (TX)/`D7` (RX) — el nodo quedaría con BME280 + GPS, pero **sin poder agregar batería monitoreada después** (no quedaría ningún pin ADC libre).
3. **DS18B20** (sonda de suelo, como el DOIT, §3.3): OneWire en un solo pin — `D6` o `D7` (o `D0`, pero conviene reservar `D0` para batería). Deja el otro pin libre.
4. **Combinación realista si se quiere todo:** DS18B20 en `D6` + batería (ADC) en `D0`, dejando `D7` libre — es el subset más parecido al DOIT que entra sin quedarse sin pines. GPS no entra en esta combinación (necesita los 2 pines UART).
5. **Otros usos genéricos de `D6`/`D7`** (no exclusivos de sensores agrícolas): pin de interrupción para un sensor con salida digital (PIR, reed switch, lluvia), control de un relé/MOSFET para actuador (bomba, válvula solenoide), o un segundo bus I2C/UART si algún sensor lo requiere.

Ninguna de estas está implementada — son las opciones que abren los pines libres, no trabajo hecho. Avisame cuál te interesa y la agrego siguiendo el mismo patrón que ya usa el emisor DOIT (flag de habilitado, persistencia NVS, comando remoto `enable_sensor` si corresponde).

## 10. Setup del repo (clonar y usar)

```
git clone <url-de-este-repo>
cd agroESP32_firmware
```

Cada nodo necesita su `secrets.h` (no versionado, ver `.gitignore`). Copiar la plantilla y completar:

```
cp src/emisor/secrets.h.example        src/emisor/secrets.h
cp src/emisor_xiao/secrets.h.example   src/emisor_xiao/secrets.h
cp src/receptor/secrets.h.example      src/receptor/secrets.h
```

- `src/emisor/secrets.h` y `src/emisor_xiao/secrets.h`: solo `DEVICE_ID_SECRET` y `SLEEP_MINUTES`.
- `src/receptor/secrets.h`: además de `device_id`, necesita `WIFI_SSID_SECRET`, `WIFI_PASS_SECRET`, `API_URL_SECRET`, `API_TOKEN_SECRET` (el token real se obtiene desde el dashboard, `/devices/<id>/secrets/`).

Sin `secrets.h`, cualquiera de los tres firmwares arranca igual en **modo provisioning** (espera config por serial, ver §4.1/§4.3) — no es obligatorio para poder compilar/flashear.
