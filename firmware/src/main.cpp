// ICE-SENSE node, edge-first.
//
//   sensor task (core 1): read LM75B / MCP3426 on Wire and FDC2214 on Wire1 -> aggregate one window
//   main loop           : web server, optional MQTT, detection (icedetect), relay, history
//
// The device detects ice by itself and works without any server. The technician reaches it at
// http://<ip>/ (access point "icesense-xxxx" is always on). MQTT is optional (research phase).
// Detection logic: lib/icedetect (C++ port of algorithm/icedetect.py).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <sys/time.h>
#include <deque>

#include "config.h"
#include "fdc2214.h"
#include "icedetect.h"
#include "lm75b.h"
#include "mcp3426.h"
#include "resistive_model.h"
#include "secrets.h"

#if MQTT_ENABLED
  #include <PubSubClient.h>
  #ifdef MQTT_USE_TLS
    #include <WiFiClientSecure.h>
    static WiFiClientSecure netClient;
  #else
    static WiFiClient netClient;
  #endif
  static PubSubClient mqtt(netClient);
  static bool mqttUp() { return mqtt.connected(); }
#else
  static bool mqttUp() { return false; }
#endif

using icedetect::State;
static Preferences prefs;
static WebServer web(80);
static DNSServer dnsServer;
static LM75B lm75(Wire, LM75B_I2C_ADDRESS);
static MCP3426 mcp3426(Wire, MCP3426_I2C_ADDRESS);
static FDC2214 fdc2214(Wire1, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);

// ============================================================================ settings
struct Settings {
  uint32_t version = 0;
  uint32_t windowMs = DEFAULT_WINDOW_S * 1000UL;
  float capBase = NAN;      // FDC2214 channel-0 code of the clean sensor; NAN = never captured
  float resBase = NAN;      // mean front/back MCP3426 differential voltage at baseline (informational)
  uint64_t baseTs = 0;
  bool relayEnabled = false;   // default OFF: the relay never switches until a technician enables it
} cfg;

static icedetect::Detector det;
static volatile uint32_t gWindowMs = DEFAULT_WINDOW_S * 1000UL;   // read by the sensor task

// Parameter table: API name, NVS key (<= 15 chars), member, limits. Mirrors PARAM_LIMITS in
// algorithm/icedetect.py (tests/test_firmware_sync.py checks this). Keep one entry per line.
struct ParamDef { const char* name; const char* key; double icedetect::Params::* d; int icedetect::Params::* i; double lo, hi; };
using P = icedetect::Params;
static const ParamDef PARAMS[] = {
  {"freeze_c", "p_freeze_c", &P::freeze_c, nullptr, -30.0, 10.0},
  {"deposit_pct", "p_deposit_pct", &P::deposit_pct, nullptr, 0.5, 100.0},
  {"ice_pct", "p_ice_pct", &P::ice_pct, nullptr, 1.0, 500.0},
  {"liquid_log10", "p_liquid_log10", &P::liquid_log10, nullptr, 2.0, 8.0},
  {"mm_per_pct", "p_mm_per_pct", &P::mm_per_pct, nullptr, 0.001, 10.0},
  {"max_mm", "p_max_mm", &P::max_mm, nullptr, 1.0, 100.0},
  {"confirm_windows", "p_confirm_win", nullptr, &P::confirm_windows, 1, 20},
  {"defrost_on_mm", "p_defr_on_mm", &P::defrost_on_mm, nullptr, 0.1, 100.0},
  {"defrost_on_windows", "p_defr_on_win", nullptr, &P::defrost_on_windows, 1, 100},
  {"defrost_off_mm", "p_defr_off_mm", &P::defrost_off_mm, nullptr, 0.0, 100.0},
  {"defrost_off_windows", "p_defr_off_win", nullptr, &P::defrost_off_windows, 1, 100},
};
static const int NPARAMS = sizeof(PARAMS) / sizeof(PARAMS[0]);

static double paramGet(const P& p, const ParamDef& d) { return d.d ? p.*(d.d) : (double)(p.*(d.i)); }
static void paramSet(P& p, const ParamDef& d, double v) { if (d.d) p.*(d.d) = v; else p.*(d.i) = (int)v; }

// ============================================================================ state
struct Stats {              // running mean/std/min/max (Welford), no sample buffer needed
  uint32_t n = 0;
  double mean = 0, m2 = 0, mn = 0, mx = 0;
  void reset() { n = 0; mean = m2 = mn = mx = 0; }
  void add(double x) {
    n++;
    double d = x - mean;
    mean += d / n;
    m2 += d * (x - mean);
    if (n == 1 || x < mn) mn = x;
    if (n == 1 || x > mx) mx = x;
  }
  double stddev() const { return n > 1 ? sqrt(m2 / (n - 1)) : 0.0; }
};
struct RStats { uint32_t n; float mean, mn, mx, sd; };
static RStats snap(const Stats& s) { return {s.n, (float)s.mean, (float)s.mn, (float)s.mx, (float)s.stddev()}; }

struct RawWindow {          // sensor task -> main loop
  uint64_t startStamp, endStamp;
  uint32_t durationMs, tempFailures, resFrontFailures, resBackFailures;
  bool fdcFault;
  bool resFrontOpen, resFrontShort, resBackOpen, resBackShort;
  double resFrontOhm, resBackOhm, resFrontLog10, resBackLog10;
  RStats cap, resFrontV, resBackV, temp;
};

static Stats stCap, stResVFront, stResVBack, stTemp;
static uint32_t tempFailures = 0, windowStartMs = 0;
static uint32_t windowResFrontFailures = 0, windowResBackFailures = 0;
static bool windowFdcFault = false;
static uint64_t windowStartStamp = 0;
static float rollCap[ROLL_LEN], rollResVFront[ROLL_LEN], rollResVBack[ROLL_LEN];
static int rollN = 0, rollPos = 0;
static SemaphoreHandle_t rollMux;
static QueueHandle_t winQueue;
static volatile uint32_t winDropped = 0;

struct Hist { uint32_t ms; uint8_t state, defrost; float thick, cap, resl, temp, counts; };
static Hist hist[HISTORY_LEN];
static int histN = 0, histHead = 0;

struct Last {
  bool valid = false;
  State state = State::Unknown;
  double thick = 0, cap = NAN, resl = NAN, resFrontLog10 = NAN, resBackLog10 = NAN, temp = NAN, counts = NAN;
  bool defrost = false, relayOn = false;
  uint32_t sinceMs = 0;
} last;
static bool tempFaultActive = false, resFrontFaultActive = false, resBackFaultActive = false, fdcFaultActive = false;

static char deviceId[24];
static uint32_t bootCount = 0, seqCounter = 0;

struct OutMsg { String topic; String payload; };
static std::deque<OutMsg> outq;
static uint32_t txOk = 0, txFail = 0, dropped = 0, reconnects = 0;
static String cmdInbox;
static uint32_t rebootAt = 0, nextStatusMs = 0;
[[maybe_unused]] static bool bootEventSent = false;

// ============================================================================ helpers
static double rnd(double x, int decimals) { double f = pow(10.0, decimals); return round(x * f) / f; }
static bool timeSynced() { return time(nullptr) > 1700000000; }
static uint64_t epochMs() { struct timeval tv; gettimeofday(&tv, nullptr); return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000; }
static uint64_t stampMs() { return timeSynced() ? epochMs() : (uint64_t)millis(); }
[[maybe_unused]] static String topicFor(const char* suffix) { return String("ice/v1/") + deviceId + "/" + suffix; }

static const char* chipName() {
  String m = ESP.getChipModel();
  if (m.startsWith("ESP32-S3")) return "ESP32-S3";
  if (m.startsWith("ESP32-S2")) return "ESP32-S2";
  if (m.startsWith("ESP32-C3")) return "ESP32-C3";
  if (m.startsWith("ESP32")) return "ESP32";
  return "other";
}
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "external_pin";
    default: return "unknown";
  }
}
static void setNum(JsonObject o, const char* k, double v, int dec) {
  if (isnan(v)) o[k] = nullptr; else o[k] = rnd(v, dec);
}

// ============================================================================ persistence
static void loadSettings() {
  cfg.version = prefs.getUInt("cfgver", 0);
  cfg.windowMs = prefs.getUInt("winms", DEFAULT_WINDOW_S * 1000UL);
  if (cfg.windowMs < MIN_WINDOW_S * 1000UL || cfg.windowMs > MAX_WINDOW_S * 1000UL) cfg.windowMs = DEFAULT_WINDOW_S * 1000UL;
  cfg.capBase = prefs.getFloat("capbase", NAN);
  if (!(cfg.capBase > 0)) cfg.capBase = NAN;
  cfg.resBase = prefs.getFloat("resbase", NAN);
  cfg.baseTs = prefs.getULong64("basets", 0);
  cfg.relayEnabled = prefs.getUInt("relayen", 0) == 1;
  P p;
  for (int i = 0; i < NPARAMS; i++) paramSet(p, PARAMS[i], prefs.getFloat(PARAMS[i].key, (float)paramGet(p, PARAMS[i])));
  det.setParams(p);
  gWindowMs = cfg.windowMs;
}
static void saveSettings() {
  prefs.putUInt("cfgver", cfg.version);
  prefs.putUInt("winms", cfg.windowMs);
  prefs.putUInt("relayen", cfg.relayEnabled ? 1 : 0);
  prefs.putFloat("capbase", isnan(cfg.capBase) ? 0.0f : cfg.capBase);   // 0 = not captured
  prefs.putFloat("resbase", isnan(cfg.resBase) ? 0.0f : cfg.resBase);
  prefs.putULong64("basets", cfg.baseTs);
  const P& p = det.params();
  for (int i = 0; i < NPARAMS; i++) prefs.putFloat(PARAMS[i].key, (float)paramGet(p, PARAMS[i]));
}

// ============================================================================ sensors (sensor task)
static void scanI2c(TwoWire& bus, const char* busName) {
  uint8_t found = 0;
  for (uint8_t address = 1; address < 0x7F; address++) {
    bus.beginTransmission(address);
    if (bus.endTransmission() == 0) {
      Serial.printf("I2C %s found 0x%02X\n", busName, address);
      found++;
    }
  }
  if (!found) Serial.printf("I2C %s found no devices\n", busName);
}
static void initSensors() {
  pinMode(PIN_FDC2214_INTB, INPUT);
  Wire.begin(PIN_I2C_A_SDA, PIN_I2C_A_SCL, I2C_CLOCK_HZ);
  Wire1.begin(PIN_I2C_B_SDA, PIN_I2C_B_SCL, I2C_CLOCK_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  Wire1.setTimeOut(I2C_TIMEOUT_MS);
  scanI2c(Wire, "Wire");
  scanI2c(Wire1, "Wire1");

  LM75B::Error temperatureError;
  if (!lm75.begin(I2C_TIMEOUT_MS, temperatureError))
    Serial.printf("LM75B 0x%02X init failed (%u)\n", LM75B_I2C_ADDRESS, (unsigned)temperatureError);

  bool mcpAddressFound = false;
  for (uint8_t address = MCP3426_I2C_ADDRESS; address <= 0x6F; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      mcp3426.setAddress(address);
      Serial.printf("MCP3426 selected address 0x%02X\n", address);
      mcpAddressFound = true;
      break;
    }
  }
  if (!mcpAddressFound) Serial.println("MCP3426 not found in address range 0x68-0x6F");
  MCP3426::Error adcError;
  if (!mcp3426.begin(I2C_TIMEOUT_MS, adcError))
    Serial.printf("MCP3426 0x%02X reset failed (%u)\n", mcp3426.address(), (unsigned)adcError);
  FDC2214::Config fdcConfig{FDC2214_RCOUNT_CH0, FDC2214_SETTLECOUNT_CH0,
                            FDC2214_CLOCK_DIVIDERS_CH0, FDC2214_DRIVE_CURRENT_CH0,
                            FDC2214_CONFIG_CH0, FDC2214_MUX_CONFIG_CH0,
                            FDC2214_ERROR_CONFIG, FDC2214_TANK_SETTINGS_VALID != 0};
  FDC2214::Error fdcError;
  uint8_t fdcAddress = FDC2214_I2C_ADDRESS_LOW;
  bool fdcReady = fdc2214.begin(fdcConfig, I2C_TIMEOUT_MS, FDC2214_POWERUP_DELAY_MS,
                                FDC2214_RESET_DELAY_MS, fdcError);
  if (!fdcReady && fdcError != FDC2214::Error::NotConfigured &&
      fdcError != FDC2214::Error::InvalidConfig) {
    fdcAddress = FDC2214_I2C_ADDRESS_HIGH;
    fdc2214.setAddress(FDC2214_I2C_ADDRESS_HIGH);
    fdcReady = fdc2214.begin(fdcConfig, I2C_TIMEOUT_MS, FDC2214_POWERUP_DELAY_MS,
                             FDC2214_RESET_DELAY_MS, fdcError);
  }
  if (!fdcReady) {
    if (fdcError == FDC2214::Error::NotConfigured)
      Serial.printf("FDC2214 detected at 0x%02X but tank settings are not configured\n", fdcAddress);
    else
      Serial.printf("FDC2214 init failed at 0x%02X (%u)\n", fdcAddress, (unsigned)fdcError);
  }
}

static resistive_model::Model resistanceModel() {
  return {RES_VDD_V, RES_R_FIXED_OHM, RES_R_REF_TOP_OHM, RES_R_REF_BOTTOM_OHM,
          MCP3426_ZIN_DIFF_OHM_X1, MCP3426_ZIN_CM_OHM, RES_VDIFF_OFFSET_V,
          RES_MIN_OHM, RES_MAX_OHM};
}

static double combineResistance(double frontOhm, double backOhm) {
  // TODO: the lower resistance is assumed to represent the wetter side; confirm this fusion rule on real data.
  const bool frontValid = isfinite(frontOhm) && frontOhm > 0;
  const bool backValid = isfinite(backOhm) && backOhm > 0;
  if (frontValid && backValid) return log10(frontOhm < backOhm ? frontOhm : backOhm);
  if (frontValid) return log10(frontOhm);
  if (backValid) return log10(backOhm);
  return NAN;
}

static double capacitiveDeltaPct(double baselineCode, double windowCode) {
  if (!(baselineCode > 0) || !(windowCode > 0) || !isfinite(baselineCode) || !isfinite(windowCode)) return NAN;
  const double ratio = baselineCode / windowCode;
  return (ratio * ratio - 1.0) * 100.0;
}

static bool readResistiveVoltage(uint8_t channel, float& volts) {
  int32_t code = 0;
  bool saturated = false;
  MCP3426::Error error;
  return mcp3426.readVoltage(channel, MCP3426_RESOLUTION_BITS, MCP3426_GAIN,
                             MCP3426_CONTINUOUS, MCP3426_CONVERSION_TIMEOUT_MS,
                             MCP3426_REFERENCE_V, code, volts, saturated, error);
}

static void resetWindow() {
  stCap.reset(); stResVFront.reset(); stResVBack.reset(); stTemp.reset();
  tempFailures = 0;
  windowResFrontFailures = windowResBackFailures = 0;
  windowFdcFault = false;
  windowStartMs = millis();
  windowStartStamp = stampMs();
}

// One sample; closes the window when its time is up. Called by the sensor task (or by the host test).
static void sensorTick() {
  uint32_t capCode = 0;
  FDC2214::Error fdcError;
  const bool capOk = fdc2214.readCode(FDC2214_CONVERSION_TIMEOUT_MS, capCode, fdcError);
  if (capOk) stCap.add(capCode); else windowFdcFault = true;

  float frontVolts = NAN, backVolts = NAN;
  if (readResistiveVoltage(MCP3426_CHANNEL_FRONT, frontVolts)) stResVFront.add(frontVolts);
  else windowResFrontFailures++;
  if (readResistiveVoltage(MCP3426_CHANNEL_BACK, backVolts)) stResVBack.add(backVolts);
  else windowResBackFailures++;

  float temperatureC = NAN;
  LM75B::Error temperatureError;
  if (lm75.readTemperatureC(temperatureC, temperatureError)) stTemp.add(temperatureC);
  else tempFailures++;

  xSemaphoreTake(rollMux, portMAX_DELAY);
  rollCap[rollPos] = capOk ? (float)capCode : NAN;
  rollResVFront[rollPos] = isfinite(frontVolts) ? frontVolts : NAN;
  rollResVBack[rollPos] = isfinite(backVolts) ? backVolts : NAN;
  rollPos = (rollPos + 1) % ROLL_LEN;
  if (rollN < ROLL_LEN) rollN++;
  xSemaphoreGive(rollMux);

  if (millis() - windowStartMs >= gWindowMs) {
    RawWindow w{};
    w.startStamp = windowStartStamp; w.endStamp = stampMs(); w.durationMs = millis() - windowStartMs;
    w.tempFailures = tempFailures; w.resFrontFailures = windowResFrontFailures;
    w.resBackFailures = windowResBackFailures; w.fdcFault = windowFdcFault;
    w.resFrontOhm = w.resBackOhm = w.resFrontLog10 = w.resBackLog10 = NAN;
    w.cap = snap(stCap); w.resFrontV = snap(stResVFront); w.resBackV = snap(stResVBack); w.temp = snap(stTemp);
    if (xQueueSend(winQueue, &w, 0) != pdTRUE) winDropped++;
    resetWindow();
  }
}
#ifndef ICE_HOST_TEST
static void sensorTask(void*) {
  const TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
  initSensors();
  TickType_t wake = xTaskGetTickCount();
  for (;;) { sensorTick(); vTaskDelayUntil(&wake, period); }
}
#endif

// ============================================================================ outputs / history
static void relayApply() {
  last.relayOn = cfg.relayEnabled && last.defrost;
  digitalWrite(PIN_RELAY, last.relayOn ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}
static void histPush(const Hist& h) {
  hist[(histHead + histN) % HISTORY_LEN] = h;
  if (histN < HISTORY_LEN) histN++; else histHead = (histHead + 1) % HISTORY_LEN;
}

// ============================================================================ messages (optional MQTT)
static void beginMessage(JsonDocument& doc, const char* type) {
  uint32_t s = seqCounter++;
  doc["schema_version"] = "1.0.0";
  doc["message_type"] = type;
  char id[64];
  snprintf(id, sizeof id, "%s-%lu-%lu", deviceId, (unsigned long)bootCount, (unsigned long)s);
  doc["message_id"] = String(id);
  doc["device_id"] = deviceId;
  doc["seq"] = s;
  doc["boot_count"] = bootCount;
  doc["ts"] = stampMs();
  doc["ts_source"] = timeSynced() ? "ntp" : "uptime_only";
  doc["uptime_ms"] = (uint64_t)millis();
  doc["config_version"] = cfg.version;
  if (!mqttUp()) doc["buffered"] = true;
}
static void addFirmware(JsonDocument& doc) {
  JsonObject f = doc["firmware"].to<JsonObject>();
  f["version"] = FW_VERSION;
  f["chip"] = chipName();
  char sdk[32];
  snprintf(sdk, sizeof sdk, "%.31s", ESP.getSdkVersion());
  f["idf_version"] = String(sdk);
}
static void enqueue(const char* type, const JsonDocument& doc) {
#if MQTT_ENABLED
  OutMsg m;
  m.topic = topicFor(type);
  serializeJson(doc, m.payload);
  if (doc.overflowed() || m.payload.length() + m.topic.length() + 16 > MQTT_BUFFER_BYTES) { dropped++; return; }
  if (outq.size() >= OUTQ_MAX) { outq.pop_front(); dropped++; }
  outq.push_back(std::move(m));
#else
  (void)type; (void)doc;
#endif
}
static void flushQueue() {
#if MQTT_ENABLED
  int budget = 3;
  while (budget-- > 0 && mqtt.connected() && !outq.empty()) {
    OutMsg& m = outq.front();
    if (mqtt.publish(m.topic.c_str(), m.payload.c_str())) { txOk++; outq.pop_front(); } else { txFail++; break; }
  }
#endif
}
static void addQuality(JsonObject ch, const char* status, const char* const* flags, int n) {
  JsonObject q = ch["quality"].to<JsonObject>();
  q["status"] = status;
  if (n > 0) { JsonArray a = q["flags"].to<JsonArray>(); for (int i = 0; i < n; i++) a.add(flags[i]); }
}
static void addStats(JsonObject ch, const RStats& s, int dec) {
  JsonObject o = ch["statistics"].to<JsonObject>();
  o["n"] = s.n; o["mean"] = rnd(s.mean, dec); o["min"] = rnd(s.mn, dec); o["max"] = rnd(s.mx, dec); o["std"] = rnd(s.sd, dec + 1);
}
static void addIce(JsonObject parent, const char* prev = nullptr) {
  JsonObject a = parent["ice_assessment"].to<JsonObject>();
  a["state"] = icedetect::stateName(last.state);
  if (prev) a["previous_state"] = prev;
  a["thickness_mm_est"] = rnd(last.thick, 2);
  a["defrost_recommended"] = last.defrost;
  a["state_duration_s"] = (millis() - last.sinceMs) / 1000;
  JsonObject al = a["algorithm"].to<JsonObject>();
  al["name"] = ICEDETECT_NAME; al["version"] = ICEDETECT_VERSION; al["type"] = "rule_based";
  JsonObject f = a["features"].to<JsonObject>();
  if (!isnan(last.cap)) f["cap_delta_pct"] = rnd(last.cap, 2);
  if (!isnan(last.resl)) f["res_log10"] = rnd(last.resl, 3);
  if (!isnan(last.temp)) f["temp_c"] = rnd(last.temp, 2);
}
static void sendEvent(const char* type, const char* severity, const char* lifecycle, const char* msg, const char* prevState = nullptr) {
  JsonDocument doc;
  beginMessage(doc, "event");
  JsonObject e = doc["event"].to<JsonObject>();
  e["event_type"] = type; e["severity"] = severity; e["state"] = lifecycle; e["message"] = msg;
  if (prevState) addIce(e, prevState);
  enqueue("event", doc);
}

static void addResistiveChannel(JsonArray channels, const char* channelId, const RStats& voltage,
                                double resistanceOhm, double log10Resistance, bool openCircuit,
                                bool shortCircuit, uint32_t readFailures) {
  JsonObject channel = channels.add<JsonObject>();
  channel["channel_id"] = channelId;
  channel["kind"] = "resistive";
  JsonObject values = channel["resistive"].to<JsonObject>();
  if (voltage.n) values["voltage_v"] = rnd(voltage.mean, 4);
  if (isfinite(resistanceOhm)) values["resistance_ohm"] = rnd(resistanceOhm, 0);
  if (isfinite(log10Resistance)) values["log10_resistance"] = rnd(log10Resistance, 3);
  if (openCircuit) values["open_circuit"] = true;
  if (shortCircuit) values["short_circuit"] = true;

  const char* flags[3];
  int flagCount = 0;
  if (openCircuit) flags[flagCount++] = "open_circuit";
  if (shortCircuit) flags[flagCount++] = "short_circuit";
  if (readFailures) flags[flagCount++] = "sensor_disconnected";
  const char* quality = !voltage.n ? "bad" :
      ((shortCircuit || readFailures || !isfinite(resistanceOhm)) ? "uncertain" : "good");
  addQuality(channel, quality, flags, flagCount);
}

static void sendTelemetry(const RawWindow& w) {
  JsonDocument doc;
  beginMessage(doc, "telemetry");
  JsonObject t = doc["telemetry"].to<JsonObject>();
  JsonObject win = t["window"].to<JsonObject>();
  win["start_ts"] = w.startStamp; win["end_ts"] = w.endStamp; win["duration_ms"] = w.durationMs;
  win["sample_rate_hz"] = SAMPLE_RATE_HZ; win["sample_count"] = w.cap.n; win["aggregation"] = "mean"; win["trigger"] = "periodic";
  JsonArray chs = t["channels"].to<JsonArray>();
  bool haveBase = !isnan(cfg.capBase) && cfg.capBase > 0;
  {
    JsonObject c = chs.add<JsonObject>();
    c["channel_id"] = "cap-1"; c["kind"] = "capacitive";
    JsonObject k = c["capacitive"].to<JsonObject>();
    if (w.cap.n) k["raw_counts"] = rnd(w.cap.mean, 1);
    if (haveBase && w.cap.n) k["delta_pct"] = rnd(capacitiveDeltaPct(cfg.capBase, w.cap.mean), 2);
    addStats(c, w.cap, 1);
    const char* fl[2]; int nf = 0;
    if (!haveBase) fl[nf++] = "baseline_invalid";
    if (!w.cap.n) fl[nf++] = "sensor_disconnected";
    addQuality(c, !w.cap.n ? "bad" : (haveBase ? "good" : "uncertain"), fl, nf);
  }
  addResistiveChannel(chs, "res-1", w.resFrontV, w.resFrontOhm, w.resFrontLog10,
                      w.resFrontOpen, w.resFrontShort, w.resFrontFailures);
  addResistiveChannel(chs, "res-2", w.resBackV, w.resBackOhm, w.resBackLog10,
                      w.resBackOpen, w.resBackShort, w.resBackFailures);
  {
    JsonObject c = chs.add<JsonObject>();
    c["channel_id"] = "temp-coil"; c["kind"] = "temperature";
    JsonObject th = c["thermal"].to<JsonObject>();
    th["sensor_type"] = "lm75b"; th["location"] = "coil_surface";
    if (w.temp.n > 0) {
      th["temperature_c"] = rnd(w.temp.mean, 2);
      th["below_freezing"] = w.temp.mean <= 0.0;
      addStats(c, w.temp, 2);
      addQuality(c, "good", nullptr, 0);
    } else {
      const char* fl[] = {"sensor_disconnected"};
      addQuality(c, "bad", fl, 1);
    }
  }
  addIce(t);
  JsonObject s = t["status_summary"].to<JsonObject>();
  s["rssi_dbm"] = WiFi.RSSI() < 0 ? WiFi.RSSI() : -127;
  s["free_heap_bytes"] = ESP.getFreeHeap();
  s["queue_depth"] = (uint32_t)outq.size();
  enqueue("telemetry", doc);
}

static void sendStatus() {
  JsonDocument doc;
  beginMessage(doc, "status");
  addFirmware(doc);
  JsonObject st = doc["status"].to<JsonObject>();
  st["health"] = (tempFaultActive || fdcFaultActive || resFrontFaultActive || resBackFaultActive) ? "degraded" : "ok";
  JsonObject c = st["connectivity"].to<JsonObject>();
  c["link"] = "wifi"; c["connected"] = mqttUp(); c["rssi_dbm"] = WiFi.RSSI() < 0 ? WiFi.RSSI() : -127;
  c["transport"] = "mqtt"; c["broker_connected"] = mqttUp(); c["reconnect_count"] = reconnects;
  c["tx_messages"] = txOk; c["tx_failures"] = txFail;
#ifdef MQTT_USE_TLS
  c["tls"] = true;
#else
  c["tls"] = false;
#endif
  JsonObject sys = st["system"].to<JsonObject>();
  sys["free_heap_bytes"] = ESP.getFreeHeap(); sys["min_free_heap_bytes"] = ESP.getMinFreeHeap();
  sys["cpu_freq_mhz"] = ESP.getCpuFreqMHz(); sys["reset_reason"] = resetReasonStr();
  JsonObject b = st["buffering"].to<JsonObject>();
  b["storage"] = "ram"; b["queued_messages"] = (uint32_t)outq.size(); b["capacity_messages"] = OUTQ_MAX;
  b["dropped_messages"] = dropped + winDropped; b["overflow_policy"] = "drop_oldest";
  JsonArray sensors = st["sensors"].to<JsonArray>();
  bool capBase = !isnan(cfg.capBase) && cfg.capBase > 0;
  JsonObject s1 = sensors.add<JsonObject>(); s1["channel_id"] = "cap-1"; s1["state"] = fdcFaultActive ? "fault" : "ok"; s1["baseline_valid"] = capBase;
  JsonObject s2 = sensors.add<JsonObject>(); s2["channel_id"] = "res-1"; s2["state"] = resFrontFaultActive ? "degraded" : "ok";
  JsonObject s3 = sensors.add<JsonObject>(); s3["channel_id"] = "res-2"; s3["state"] = resBackFaultActive ? "degraded" : "ok";
  JsonObject s4 = sensors.add<JsonObject>(); s4["channel_id"] = "temp-coil"; s4["state"] = tempFaultActive ? "fault" : "ok";
  enqueue("status", doc);
}

// ============================================================================ detection per window (main loop)
static void processWindow(RawWindow& w) {
  const resistive_model::Model model = resistanceModel();
  if (w.resFrontV.n) {
    resistive_model::Estimate estimate;
    if (resistive_model::resistanceFromVoltageDiff(w.resFrontV.mean, MCP3426_GAIN, model, estimate)) {
      w.resFrontOhm = estimate.resistanceOhm;
      w.resFrontLog10 = log10(estimate.resistanceOhm);
      w.resFrontOpen = estimate.openCircuit;
      w.resFrontShort = estimate.shortCircuit;
    } else w.resFrontFailures++;
  }
  if (w.resBackV.n) {
    resistive_model::Estimate estimate;
    if (resistive_model::resistanceFromVoltageDiff(w.resBackV.mean, MCP3426_GAIN, model, estimate)) {
      w.resBackOhm = estimate.resistanceOhm;
      w.resBackLog10 = log10(estimate.resistanceOhm);
      w.resBackOpen = estimate.openCircuit;
      w.resBackShort = estimate.shortCircuit;
    } else w.resBackFailures++;
  }

  bool haveBase = !isnan(cfg.capBase) && cfg.capBase > 0;
  icedetect::Input in;
  if (haveBase && w.cap.n) in.cap_delta_pct = capacitiveDeltaPct(cfg.capBase, w.cap.mean);
  in.res_log10 = combineResistance(w.resFrontOhm, w.resBackOhm);
  if (w.temp.n) in.temp_c = w.temp.mean;
#if FDC2214_DEBUG
  if (w.cap.n) {
    const uint16_t finSelect = (FDC2214_CLOCK_DIVIDERS_CH0 >> 12) & 0x03;
    const uint8_t inputDivider = finSelect == 1 ? 1 : (finSelect == 2 ? 2 : 0);
    const uint16_t referenceDivider = FDC2214_CLOCK_DIVIDERS_CH0 & 0x03FF;
    float frequencyHz = NAN, capacitancePf = NAN;
    const uint32_t code = (uint32_t)w.cap.mean;
    if (inputDivider && referenceDivider) {
      FDC2214::Error debugError;
      FDC2214::codeToFrequency(code, FDC2214_REFERENCE_CLOCK_HZ, referenceDivider,
                               inputDivider, frequencyHz, debugError);
      FDC2214::codeToCapacitancePf(code, FDC2214_REFERENCE_CLOCK_HZ, referenceDivider,
                                   inputDivider, FDC_TANK_L_UH, FDC_TANK_C_PF,
                                   capacitancePf, debugError);
    }
    Serial.printf("FDC2214 code=%lu frequency_hz=%.3f capacitance_pf=%.3f\n",
                  (unsigned long)code, frequencyHz, capacitancePf);
  }
#endif

  const bool resFrontFault = w.resFrontShort || w.resFrontFailures > 0;
  const bool resBackFault = w.resBackShort || w.resBackFailures > 0;

  icedetect::Output out = det.step(in);
  State prev = last.state;
  bool prevDefrost = last.defrost;
  if (!last.valid || out.state != prev) last.sinceMs = millis();
  last.valid = true; last.state = out.state; last.thick = out.thickness_mm; last.defrost = out.defrost_request;
  last.cap = in.cap_delta_pct; last.resl = in.res_log10;
  last.resFrontLog10 = w.resFrontLog10; last.resBackLog10 = w.resBackLog10; last.temp = in.temp_c;
  last.counts = w.cap.n ? w.cap.mean : NAN;
  relayApply();

  Serial.printf("%s thick=%.2f mm defrost=%d cap=%.2f%% R=%.2f temp=%.1f counts=%.0f\n", icedetect::stateName(last.state), last.thick, last.defrost, last.cap, last.resl, last.temp, last.counts);

  Hist h{millis(), (uint8_t)out.state, (uint8_t)out.defrost_request, (float)out.thickness_mm,
         (float)in.cap_delta_pct, (float)in.res_log10, (float)in.temp_c, w.cap.mean};
  histPush(h);

  bool tempFault = w.temp.n == 0;
  if (tempFault != tempFaultActive) {
    tempFaultActive = tempFault;
    sendEvent("sensor_fault", tempFault ? "warning" : "info", tempFault ? "raised" : "cleared", "coil temperature sensor");
  }
  const bool fdcFault = w.fdcFault || w.cap.n == 0;
  if (fdcFault != fdcFaultActive) {
    fdcFaultActive = fdcFault;
    sendEvent("sensor_fault", fdcFault ? "warning" : "info", fdcFault ? "raised" : "cleared", "FDC2214 capacitive sensor");
  }
  if (resFrontFault != resFrontFaultActive || resBackFault != resBackFaultActive) {
    resFrontFaultActive = resFrontFault;
    resBackFaultActive = resBackFault;
    const bool resFault = resFrontFault || resBackFault;
    sendEvent("sensor_fault", resFault ? "warning" : "info", resFault ? "raised" : "cleared",
              "resistive sensor short or MCP3426 read failure");
  }
  if (out.state != prev) {
    char m[64];
    snprintf(m, sizeof m, "%s -> %s", icedetect::stateName(prev), icedetect::stateName(out.state));
    sendEvent("ice_state_changed", "info", "instant", m, icedetect::stateName(prev));
  }
  if (out.defrost_request != prevDefrost)
    sendEvent("defrost_recommended", out.defrost_request ? "warning" : "info", out.defrost_request ? "raised" : "cleared",
              out.defrost_request ? "defrost requested" : "defrost request released");
  sendTelemetry(w);
}

// ============================================================================ operations shared by web and MQTT
static bool applyParams(JsonObjectConst o, String& err) {
  P p = det.params();
  for (JsonPairConst kv : o) {
    const ParamDef* d = nullptr;
    for (int i = 0; i < NPARAMS; i++) if (!strcmp(PARAMS[i].name, kv.key().c_str())) d = &PARAMS[i];
    if (!d) { err = String("unknown parameter '") + kv.key().c_str() + "'"; return false; }
    if (kv.value().is<bool>() || !kv.value().is<double>()) { err = String("'") + d->name + "' must be a number"; return false; }
    double v = kv.value().as<double>();
    if (!isfinite(v) || v < d->lo || v > d->hi) { err = String("'") + d->name + "' out of range"; return false; }
    if (d->i && v != floor(v)) { err = String("'") + d->name + "' must be an integer"; return false; }
    paramSet(p, *d, v);
  }
  if (p.defrost_off_mm >= p.defrost_on_mm) { err = "defrost_off_mm must be below defrost_on_mm (hysteresis)"; return false; }
  det.setParams(p);
  cfg.version++;
  saveSettings();
  return true;
}
static bool captureBaseline(String& err) {
  xSemaphoreTake(rollMux, portMAX_DELAY);
  int n = rollN, capN = 0, resN = 0;
  double a = 0, b = 0;
  for (int i = 0; i < n; i++) {
    if (isfinite(rollCap[i])) { a += rollCap[i]; capN++; }
    if (isfinite(rollResVFront[i])) { b += rollResVFront[i]; resN++; }
    if (isfinite(rollResVBack[i])) { b += rollResVBack[i]; resN++; }
  }
  xSemaphoreGive(rollMux);
  if (capN < BASELINE_MIN_SAMPLES) { err = "not enough capacitive samples yet, retry in a few seconds"; return false; }
  cfg.capBase = (float)(a / capN);
  cfg.resBase = resN ? (float)(b / resN) : NAN;
  cfg.baseTs = stampMs();
  cfg.version++;
  saveSettings();
  det.reset();
  last.state = State::Unknown; last.defrost = false; last.thick = 0;
  relayApply();
  sendEvent("baseline_updated", "notice", "instant", "baseline captured");
  return true;
}
static void setRelay(bool en) { cfg.relayEnabled = en; cfg.version++; saveSettings(); relayApply(); }

// ============================================================================ web server
static void stateJson(String& out) {
  JsonDocument d;
  d["device_id"] = deviceId; d["simulated"] = false; d["fw"] = FW_VERSION;
  d["algorithm"]["name"] = ICEDETECT_NAME; d["algorithm"]["version"] = ICEDETECT_VERSION;
  d["uptime_s"] = millis() / 1000; d["boot_count"] = bootCount; d["seq"] = seqCounter;
  d["state"] = (int)last.state; d["state_name"] = icedetect::stateName(last.state);
  d["thickness_mm"] = rnd(last.thick, 3); d["thickness_calibrated"] = false; d["defrost_request"] = last.defrost;
  JsonObject r = d["raw"].to<JsonObject>();
  setNum(r, "cap_delta_pct", last.cap, 3); setNum(r, "cap_counts", last.counts, 1);
  setNum(r, "res_log10", last.resl, 4);
  setNum(r, "res_front_log10", last.resFrontLog10, 4);
  setNum(r, "res_back_log10", last.resBackLog10, 4);
  setNum(r, "temp_c", last.temp, 3);
  d["baseline"]["captured"] = !isnan(cfg.capBase) && cfg.capBase > 0;
  d["baseline"]["cap_counts"] = isnan(cfg.capBase) ? 0.0 : (double)cfg.capBase;
  d["relay"]["enabled"] = cfg.relayEnabled; d["relay"]["on"] = last.relayOn;
  JsonObject pr = d["params"].to<JsonObject>();
  for (int i = 0; i < NPARAMS; i++) pr[PARAMS[i].name] = paramGet(det.params(), PARAMS[i]);
  d["net"]["sta_connected"] = WiFi.status() == WL_CONNECTED; d["net"]["mqtt_connected"] = mqttUp();
  serializeJson(d, out);
}
static bool authed() {
  if (web.authenticate(UI_USER, UI_PASS)) return true;
  web.requestAuthentication();
  return false;
}
static void sendState(int code = 200) { String s; stateJson(s); web.send(code, "application/json", s); }
static bool bodyJson(JsonDocument& d) {
  if (web.arg("plain").length() > 1024) { web.send(400, "text/plain", "body too large"); return false; }
  if (web.arg("plain").length() && deserializeJson(d, web.arg("plain"))) { web.send(400, "text/plain", "invalid JSON"); return false; }
  return true;
}
static void historyCsv() {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/csv", "");
  web.sendContent("uptime_s,state,state_name,thickness_mm,defrost_request,cap_delta_pct,res_log10,temp_c,cap_counts\n");
  String chunk;
  char line[160];
  for (int i = 0; i < histN; i++) {
    const Hist& h = hist[(histHead + i) % HISTORY_LEN];
    auto f = [](char* b, size_t n, float v, const char* fmt) { if (isnan(v)) b[0] = 0; else snprintf(b, n, fmt, v); };
    char c1[16], c2[16], c3[16];
    f(c1, sizeof c1, h.cap, "%.3f"); f(c2, sizeof c2, h.resl, "%.4f"); f(c3, sizeof c3, h.temp, "%.3f");
    snprintf(line, sizeof line, "%.1f,%u,%s,%.3f,%u,%s,%s,%s,%.1f\n", h.ms / 1000.0, h.state,
             icedetect::stateName((State)h.state), h.thick, h.defrost, c1, c2, c3, h.counts);
    chunk += line;
    if (chunk.length() > 1200) { web.sendContent(chunk); chunk = ""; }
  }
  if (chunk.length()) web.sendContent(chunk);
  web.sendContent("");
}
static void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (!authed()) return;
    if (!LittleFS.exists("/index.html.gz")) { web.send(200, "text/plain", "UI not uploaded. Run: pio run -t uploadfs (the /api endpoints work)."); return; }
    File f = LittleFS.open("/index.html.gz", "r");
    web.streamFile(f, "text/html");
    f.close();
  });
  auto redirectToRoot = []() {
    web.sendHeader("Location", "/");
    web.send(302, "text/plain", "");
  };
  web.on("/generate_204", HTTP_GET, redirectToRoot);
  web.on("/hotspot-detect.html", HTTP_GET, redirectToRoot);
  web.on("/connecttest.txt", HTTP_GET, redirectToRoot);
  web.on("/redirect", HTTP_GET, []() {
    web.sendHeader("Location", "http://www.msftconnecttest.com/");
    web.send(302, "text/plain", "");
  });
  web.on("/api/state", HTTP_GET, []() { if (authed()) sendState(); });
  web.on("/api/history.csv", HTTP_GET, []() { if (authed()) historyCsv(); });
  web.on("/api/baseline", HTTP_POST, []() {
    if (!authed()) return;
    String err;
    if (captureBaseline(err)) sendState(); else web.send(400, "text/plain", err);
  });
  web.on("/api/params", HTTP_POST, []() {
    if (!authed()) return;
    JsonDocument d;
    if (!bodyJson(d)) return;
    String err;
    if (!d.is<JsonObject>()) { web.send(400, "text/plain", "JSON object expected"); return; }
    if (applyParams(d.as<JsonObjectConst>(), err)) sendState(); else web.send(400, "text/plain", err);
  });
  web.on("/api/params/defaults", HTTP_POST, []() {
    if (!authed()) return;
    det.setParams(P());
    cfg.version++;
    saveSettings();
    sendState();
  });
  web.on("/api/relay", HTTP_POST, []() {
    if (!authed()) return;
    JsonDocument d;
    if (!bodyJson(d)) return;
    if (!d["enabled"].is<bool>()) { web.send(400, "text/plain", "'enabled' must be true or false"); return; }
    setRelay(d["enabled"].as<bool>());
    sendState();
  });
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
}

// ============================================================================ MQTT commands
static void sendAck(const char* cmdId, const char* result, const char* errCode = nullptr, const char* msg = nullptr, bool withCfg = false) {
  JsonDocument doc;
  beginMessage(doc, "command_ack");
  doc["in_reply_to"] = cmdId;
  JsonObject a = doc["command_ack"].to<JsonObject>();
  a["command_id"] = cmdId; a["result"] = result;
  if (errCode) a["error_code"] = errCode;
  if (msg) a["message"] = msg;
  if (withCfg) a["applied_config_version"] = cfg.version;
  enqueue("command_ack", doc);
}
static void handleCommand(const String& raw) {
  JsonDocument d;
  if (deserializeJson(d, raw)) return;
  const char* cmdId = d["command"]["command_id"] | "";
  const char* type = d["command"]["command_type"] | "";
  const char* target = d["device_id"] | "";
  if (!*cmdId) return;
  if (strcmp(target, deviceId) != 0) { sendAck(cmdId, "rejected", "E_WRONG_DEVICE"); return; }
  uint64_t expires = d["command"]["expires_ts"] | (uint64_t)0;
  if (expires && timeSynced() && epochMs() > expires) { sendAck(cmdId, "expired"); return; }
  String err;
  if (!strcmp(type, "set_config")) {
    JsonObjectConst c = d["command"]["config"].as<JsonObjectConst>();
    JsonVariantConst wi = c["reporting"]["telemetry_interval_s"];
    float win = wi.isNull() ? 0 : wi.as<float>();
    if (!wi.isNull() && (!wi.is<float>() || win < MIN_WINDOW_S || win > MAX_WINDOW_S)) { sendAck(cmdId, "rejected", "E_BAD_CONFIG", "telemetry_interval_s out of range"); return; }
    JsonVariantConst pv = c["algorithm"]["parameters"];
    if (!pv.isNull() && !applyParams(pv.as<JsonObjectConst>(), err)) { sendAck(cmdId, "rejected", "E_BAD_CONFIG", err.c_str()); return; }
    if (!wi.isNull()) { cfg.windowMs = (uint32_t)(win * 1000); gWindowMs = cfg.windowMs; cfg.version++; saveSettings(); }
    sendAck(cmdId, "completed", nullptr, nullptr, true);
  } else if (!strcmp(type, "capture_baseline")) {
    if (captureBaseline(err)) sendAck(cmdId, "completed"); else sendAck(cmdId, "rejected", "E_NOT_READY", err.c_str());
  } else if (!strcmp(type, "set_output")) {
    JsonVariantConst en = d["command"]["params"]["enabled"];
    if (strcmp(d["command"]["params"]["output"] | "", "relay") || !en.is<bool>()) { sendAck(cmdId, "rejected", "E_BAD_PARAMS", "need params.output=relay and params.enabled=true|false"); return; }
    setRelay(en.as<bool>());
    sendAck(cmdId, "completed");
  } else if (!strcmp(type, "get_status")) {
    sendStatus(); sendAck(cmdId, "completed");
  } else if (!strcmp(type, "reboot")) {
    sendAck(cmdId, "accepted");
    rebootAt = millis() + 1500; if (!rebootAt) rebootAt = 1;
  } else {
    sendAck(cmdId, "unsupported", "E_UNSUPPORTED");
  }
}
#if MQTT_ENABLED
static void onMqttMessage(char*, byte* payload, unsigned int len) {
  if (cmdInbox.length() == 0) cmdInbox.concat((const char*)payload, len);
}
static void connectMqtt() {
  String online = topicFor("online");
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASS : nullptr;
  if (!mqtt.connect(deviceId, user, pass, online.c_str(), 1, true, "0")) return;
  reconnects++;
  mqtt.publish(online.c_str(), "1", true);
  mqtt.subscribe(topicFor("cmd").c_str(), 1);
  if (!bootEventSent) {
    JsonDocument doc;
    beginMessage(doc, "event");
    addFirmware(doc);
    JsonObject e = doc["event"].to<JsonObject>();
    e["event_type"] = "boot"; e["severity"] = "info"; e["state"] = "instant"; e["message"] = "device booted";
    e["details"]["reset_reason"] = resetReasonStr();
    enqueue("event", doc);
    bootEventSent = true;
  }
  nextStatusMs = millis();
}
#endif
static void serviceNetwork(uint32_t now) {
  static uint32_t lastWifiTry = 0, lastMqttTry = (uint32_t)-5000;
  if (strlen(WIFI_SSID) && WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 10000) { lastWifiTry = now; WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
#if MQTT_ENABLED
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) { if (now - lastMqttTry > 5000) { lastMqttTry = now; connectMqtt(); } return; }
  mqtt.loop();
#else
  (void)lastMqttTry;
#endif
}

// ============================================================================ arduino entry points
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, !RELAY_ACTIVE_LEVEL);          // relay open until settings say otherwise
  prefs.begin("ice", false);
  loadSettings();
  bootCount = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);
  snprintf(deviceId, sizeof deviceId, "esp32-%012llx", (unsigned long long)ESP.getEfuseMac());
  Serial.printf("\n%s fw %s boot #%lu\n", deviceId, FW_VERSION, (unsigned long)bootCount);
  if (!strcmp(UI_PASS, "change-me-now")) Serial.println("WARNING: default UI password, change it in secrets.h");

  LittleFS.begin(true);

  WiFi.mode(WIFI_AP_STA);
  char ap[40];
  snprintf(ap, sizeof ap, "%s%s", AP_SSID_PREFIX, deviceId + strlen(deviceId) - 4);
  WiFi.softAP(ap, AP_PASS);
  if (!dnsServer.start(53, "*", IPAddress(192, 168, 4, 1)))
    Serial.println("WARNING: DNS responder failed to start");
  if (strlen(WIFI_SSID)) { WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID, WIFI_PASS); configTime(0, 0, "pool.ntp.org", "time.google.com"); }
  MDNS.begin(ap);
#if MQTT_ENABLED
  #ifdef MQTT_USE_TLS
  netClient.setCACert(MQTT_CA_CERT);
  #endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
#endif
  setupWeb();

  rollMux = xSemaphoreCreateMutex();
  winQueue = xQueueCreate(WIN_QUEUE_LEN, sizeof(RawWindow));
  resetWindow();
#ifdef ICE_HOST_TEST
  initSensors();
#else
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 3, nullptr, 1);
#endif
  nextStatusMs = millis() + STATUS_INTERVAL_S * 1000UL;
}

void loop() {
  uint32_t now = millis();
  dnsServer.processNextRequest();
  web.handleClient();
  serviceNetwork(now);
  if (cmdInbox.length()) { String raw = cmdInbox; cmdInbox = ""; handleCommand(raw); }
  RawWindow w;
  while (xQueueReceive(winQueue, &w, 0) == pdTRUE) processWindow(w);
  if ((int32_t)(now - nextStatusMs) >= 0) { sendStatus(); nextStatusMs = now + STATUS_INTERVAL_S * 1000UL; }
  flushQueue();
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) ESP.restart();
  delay(2);
}
