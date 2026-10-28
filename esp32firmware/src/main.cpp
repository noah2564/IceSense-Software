// Ice-sensing node, first draft.
//
// Loop:  sample (capacitive touch pad, resistive divider, NTC)  ->  aggregate over a window
//        ->  publish one schema-conform JSON telemetry message  ->  repeat.
// Also: periodic status, boot event, RAM buffering while offline, and a small command
// channel (set_config, capture_baseline, get_status, reboot).
//
// Deliberately simple: single thread, plain DC excitation, no on-device ice decision.
// See README "Known limitations" for what comes next.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <math.h>
#include <sys/time.h>
#include <deque>

#include "config.h"
#include "secrets.h"

#ifdef MQTT_USE_TLS
  #include <WiFiClientSecure.h>
  static WiFiClientSecure netClient;
#else
  static WiFiClient netClient;
#endif
static PubSubClient mqtt(netClient);
static Preferences prefs;

// ============================================================================ state
struct Config {
  uint32_t version = 0;
  uint32_t telemetryIntervalS = DEFAULT_TELEMETRY_INTERVAL_S;
  float sampleRateHz = DEFAULT_SAMPLE_RATE_HZ;
  float capBase = NAN;      // touch counts of a clean coil; NAN = never captured
  float resBase = NAN;      // ohm of a clean coil
  uint64_t baseTs = 0;
} cfg;

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

static Stats stCap, stRes, stResV, stTemp, stNtcR;
static uint32_t tempFailures = 0;

static char deviceId[24];
static uint32_t bootCount = 0;
static uint32_t seqCounter = 0;             // restarts at 0 every boot; (boot_count, seq) is unique

struct OutMsg { String topic; String payload; };
static std::deque<OutMsg> outq;
static uint32_t txOk = 0, txFail = 0, dropped = 0, reconnects = 0;

static String cmdInbox;                     // command received in the MQTT callback, handled in loop()
static uint32_t rebootAt = 0;
static bool bootEventSent = false;

static uint32_t nextSampleMs = 0, windowStartMs = 0, nextStatusMs = 0;
static uint64_t windowStartStamp = 0;
static bool lastResOpen = false, lastResShort = false;

// ============================================================================ helpers
static double rnd(double x, int decimals) {
  double f = pow(10.0, decimals);
  return round(x * f) / f;
}

static bool timeSynced() { return time(nullptr) > 1700000000; }

static uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// Wall-clock ms once NTP has synced, otherwise uptime ms (message then says ts_source=uptime_only).
static uint64_t stampMs() { return timeSynced() ? epochMs() : (uint64_t)millis(); }

static String topicFor(const char* suffix) {
  return String(MQTT_TOPIC_PREFIX) + "/" + deviceId + "/" + suffix;
}

static const char* chipName() {
  String m = ESP.getChipModel();
  if (m.startsWith("ESP32-S3")) return "ESP32-S3";
  if (m.startsWith("ESP32-S2")) return "ESP32-S2";
  if (m.startsWith("ESP32-C3")) return "ESP32-C3";
  if (m.startsWith("ESP32-C6")) return "ESP32-C6";
  if (m.startsWith("ESP32")) return "ESP32";
  return "other";
}

static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT:      return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:      return "external_pin";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

// ============================================================================ persistence
static void loadConfig() {
  cfg.version = prefs.getUInt("cfgver", 0);
  cfg.telemetryIntervalS = prefs.getUInt("tint", DEFAULT_TELEMETRY_INTERVAL_S);
  cfg.sampleRateHz = prefs.getFloat("srate", DEFAULT_SAMPLE_RATE_HZ);
  cfg.capBase = prefs.getFloat("capbase", NAN);
  cfg.resBase = prefs.getFloat("resbase", NAN);
  cfg.baseTs = prefs.getULong64("basets", 0);
}

static void saveConfig() {
  prefs.putUInt("cfgver", cfg.version);
  prefs.putUInt("tint", cfg.telemetryIntervalS);
  prefs.putFloat("srate", cfg.sampleRateHz);
  if (!isnan(cfg.capBase)) prefs.putFloat("capbase", cfg.capBase);
  if (!isnan(cfg.resBase)) prefs.putFloat("resbase", cfg.resBase);
  prefs.putULong64("basets", cfg.baseTs);
}

// ============================================================================ sensors
static uint32_t readMvAvg(int pin, int n) {
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) sum += analogReadMilliVolts(pin);
  return sum / n;
}

static double readTouchAvg() {
  double sum = 0;
  for (int i = 0; i < 4; i++) sum += touchRead(PIN_TOUCH);
  return sum / 4.0;
}

// Excitation is ON only during the measurement: saves power and limits electrode corrosion.
// TODO: alternate polarity (needs a second drive pin or an H-bridge); the schema already has fields for it.
static void readResistive(double& rOhm, double& volts) {
  digitalWrite(PIN_RES_EXCITE, HIGH);
  delay(RES_SETTLE_MS);
  uint32_t mv = readMvAvg(PIN_RES_ADC, 8);
  digitalWrite(PIN_RES_EXCITE, LOW);
  volts = mv / 1000.0;
  double v = constrain(volts, 0.001, ADC_VCC - 0.001);
  rOhm = RES_R_REF_OHM * v / (ADC_VCC - v);   // divider: V = Vcc * R_el / (R_ref + R_el)
  rOhm = constrain(rOhm, RES_MIN_OHM, RES_MAX_OHM);
}

static bool readNtc(double& tempC, double& rOhm) {
  uint32_t mv = readMvAvg(PIN_NTC_ADC, 8);
  if (mv < 50 || mv > 3250) return false;     // open or shorted sensor
  double v = mv / 1000.0;
  rOhm = NTC_R_FIXED_OHM * v / (ADC_VCC - v);
  tempC = 1.0 / (1.0 / 298.15 + log(rOhm / NTC_R0_OHM) / NTC_BETA) - 273.15;   // beta equation
  return true;
}

static void sampleOnce() {
  stCap.add(readTouchAvg());
  double r, v;
  readResistive(r, v);
  stRes.add(r);
  stResV.add(v);
  double t, rn;
  if (readNtc(t, rn)) { stTemp.add(t); stNtcR.add(rn); }
  else tempFailures++;
}

static void resetWindow() {
  stCap.reset(); stRes.reset(); stResV.reset(); stTemp.reset(); stNtcR.reset();
  windowStartMs = millis();
  windowStartStamp = stampMs();
}

// ============================================================================ message building
static void beginMessage(JsonDocument& doc, const char* type) {
  uint32_t s = seqCounter++;
  doc["schema_version"] = SCHEMA_VERSION;
  doc["message_type"] = type;
  char id[64];
  snprintf(id, sizeof id, "%s-%lu-%lu", deviceId, (unsigned long)bootCount, (unsigned long)s);
  doc["message_id"] = String(id);            // String => ArduinoJson copies it
  doc["device_id"] = deviceId;
  doc["seq"] = s;
  doc["boot_count"] = bootCount;
  doc["ts"] = stampMs();
  doc["ts_source"] = timeSynced() ? "ntp" : "uptime_only";
  doc["uptime_ms"] = (uint64_t)millis();
  doc["config_version"] = cfg.version;
  if (!mqtt.connected()) doc["buffered"] = true;   // built while offline: will arrive late
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
  OutMsg m;
  m.topic = topicFor(type);
  serializeJson(doc, m.payload);
  if (doc.overflowed() || m.payload.length() + m.topic.length() + 16 > MQTT_BUFFER_BYTES) {
    Serial.printf("drop: %s message too large (%u B)\n", type, (unsigned)m.payload.length());
    dropped++;
    return;
  }
  if (outq.size() >= OUTQ_MAX) { outq.pop_front(); dropped++; }   // keep the newest data
  outq.push_back(std::move(m));
}

static void flushQueue() {
  int budget = 3;                                   // don't starve sampling after a long outage
  while (budget-- > 0 && mqtt.connected() && !outq.empty()) {
    OutMsg& m = outq.front();
    if (mqtt.publish(m.topic.c_str(), m.payload.c_str())) { txOk++; outq.pop_front(); }
    else { txFail++; break; }
  }
}

static void addQuality(JsonObject ch, const char* status, const char* const* flags, int nFlags) {
  JsonObject q = ch["quality"].to<JsonObject>();
  q["status"] = status;
  if (nFlags > 0) {
    JsonArray a = q["flags"].to<JsonArray>();
    for (int i = 0; i < nFlags; i++) a.add(flags[i]);
  }
}

static void addStats(JsonObject ch, const Stats& s, int decimals) {
  JsonObject o = ch["statistics"].to<JsonObject>();
  o["n"] = s.n;
  o["mean"] = rnd(s.mean, decimals);
  o["min"] = rnd(s.mn, decimals);
  o["max"] = rnd(s.mx, decimals);
  o["std"] = rnd(s.stddev(), decimals + 1);
}

static void sendTelemetry() {
  if (stCap.n == 0) return;
  JsonDocument doc;
  beginMessage(doc, "telemetry");
  JsonObject t = doc["telemetry"].to<JsonObject>();

  JsonObject w = t["window"].to<JsonObject>();
  w["start_ts"] = windowStartStamp;
  w["end_ts"] = stampMs();
  w["duration_ms"] = (uint32_t)(millis() - windowStartMs);
  w["sample_rate_hz"] = cfg.sampleRateHz;
  w["sample_count"] = stCap.n;
  w["aggregation"] = "mean";
  w["trigger"] = "periodic";

  JsonArray chs = t["channels"].to<JsonArray>();

  // ---- capacitive. Touch counts are not pF; delta_pct vs the stored baseline is what matters.
  {
    JsonObject c = chs.add<JsonObject>();
    c["channel_id"] = "cap-1";
    c["kind"] = "capacitive";
    JsonObject k = c["capacitive"].to<JsonObject>();
    k["raw_counts"] = rnd(stCap.mean, 1);
    bool haveBase = !isnan(cfg.capBase) && cfg.capBase > 0;
    if (haveBase) k["delta_pct"] = rnd((stCap.mean - cfg.capBase) / cfg.capBase * 100.0, 2);
    addStats(c, stCap, 1);
    const char* flags[] = {"baseline_invalid"};
    addQuality(c, haveBase ? "good" : "uncertain", flags, haveBase ? 0 : 1);
  }

  // ---- resistive
  {
    JsonObject c = chs.add<JsonObject>();
    c["channel_id"] = "res-1";
    c["kind"] = "resistive";
    JsonObject k = c["resistive"].to<JsonObject>();
    double mv = stResV.mean * 1000.0;
    lastResOpen = mv > RES_OPEN_MV;
    lastResShort = mv < RES_SHORT_MV;
    k["voltage_v"] = rnd(stResV.mean, 3);
    k["resistance_ohm"] = rnd(stRes.mean, 0);
    k["log10_resistance"] = rnd(log10(stRes.mean), 3);
    bool haveBase = !isnan(cfg.resBase) && cfg.resBase > 0;
    if (haveBase) {
      k["baseline_ohm"] = rnd(cfg.resBase, 0);
      k["delta_pct"] = rnd((stRes.mean - cfg.resBase) / cfg.resBase * 100.0, 2);
    }
    if (lastResOpen) k["open_circuit"] = true;
    if (lastResShort) k["short_circuit"] = true;
    addStats(c, stRes, 0);
    const char* flags[3];
    int nf = 0;
    if (lastResOpen) flags[nf++] = "open_circuit";
    if (lastResShort) flags[nf++] = "short_circuit";
    if (!haveBase) flags[nf++] = "baseline_invalid";
    addQuality(c, nf ? "uncertain" : "good", flags, nf);
  }

  // ---- coil temperature (context that separates liquid water from ice)
  {
    JsonObject c = chs.add<JsonObject>();
    c["channel_id"] = "temp-coil";
    c["kind"] = "temperature";
    JsonObject th = c["thermal"].to<JsonObject>();
    th["sensor_type"] = "ntc";
    th["location"] = "coil_surface";
    th["ntc_beta"] = NTC_BETA;
    if (stTemp.n > 0) {
      th["temperature_c"] = rnd(stTemp.mean, 2);
      th["resistance_ohm"] = rnd(stNtcR.mean, 0);
      th["below_freezing"] = stTemp.mean <= 0.0;
      addStats(c, stTemp, 2);
      addQuality(c, "good", nullptr, 0);
    } else {
      const char* flags[] = {"sensor_disconnected"};
      addQuality(c, "bad", flags, 1);
    }
  }

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
  st["health"] = (stTemp.n == 0 && tempFailures > 0) ? "degraded" : "ok";

  JsonObject c = st["connectivity"].to<JsonObject>();
  c["link"] = "wifi";
  c["connected"] = mqtt.connected();
  c["rssi_dbm"] = WiFi.RSSI() < 0 ? WiFi.RSSI() : -127;
  c["transport"] = "mqtt";
  c["broker_connected"] = mqtt.connected();
  c["reconnect_count"] = reconnects;
  c["tx_messages"] = txOk;
  c["tx_failures"] = txFail;
#ifdef MQTT_USE_TLS
  c["tls"] = true;
#else
  c["tls"] = false;
#endif

  JsonObject sys = st["system"].to<JsonObject>();
  sys["free_heap_bytes"] = ESP.getFreeHeap();
  sys["min_free_heap_bytes"] = ESP.getMinFreeHeap();
  sys["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  sys["reset_reason"] = resetReasonStr();

  JsonObject b = st["buffering"].to<JsonObject>();
  b["storage"] = "ram";
  b["queued_messages"] = (uint32_t)outq.size();
  b["capacity_messages"] = OUTQ_MAX;
  b["dropped_messages"] = dropped;
  b["overflow_policy"] = "drop_oldest";

  JsonArray sensors = st["sensors"].to<JsonArray>();
  bool capBase = !isnan(cfg.capBase), resBase = !isnan(cfg.resBase);
  JsonObject s1 = sensors.add<JsonObject>();
  s1["channel_id"] = "cap-1"; s1["state"] = "ok"; s1["baseline_valid"] = capBase;
  JsonObject s2 = sensors.add<JsonObject>();
  s2["channel_id"] = "res-1";
  s2["state"] = (lastResOpen || lastResShort) ? "degraded" : "ok";
  s2["baseline_valid"] = resBase;
  JsonObject s3 = sensors.add<JsonObject>();
  s3["channel_id"] = "temp-coil";
  s3["state"] = (stTemp.n == 0 && tempFailures > 0) ? "fault" : "ok";
  s3["error_count"] = tempFailures;

  enqueue("status", doc);
}

static void sendBootEvent() {
  JsonDocument doc;
  beginMessage(doc, "event");
  addFirmware(doc);
  JsonObject e = doc["event"].to<JsonObject>();
  e["event_type"] = "boot";
  e["severity"] = "info";
  e["state"] = "instant";
  e["message"] = "device booted";
  e["details"]["reset_reason"] = resetReasonStr();
  enqueue("event", doc);
}

static void sendAck(const char* cmdId, const char* result, const char* errCode = nullptr,
                    const char* msg = nullptr, bool withConfigVersion = false) {
  JsonDocument doc;
  beginMessage(doc, "command_ack");
  doc["in_reply_to"] = cmdId;
  JsonObject a = doc["command_ack"].to<JsonObject>();
  a["command_id"] = cmdId;
  a["result"] = result;
  if (errCode) a["error_code"] = errCode;
  if (msg) a["message"] = msg;
  if (withConfigVersion) a["applied_config_version"] = cfg.version;
  enqueue("command_ack", doc);
}

static void sendCalibration(const char* channel, const char* unit, double base, const Stats& s,
                            bool haveTemp, double tempC) {
  JsonDocument doc;
  beginMessage(doc, "calibration");
  JsonObject c = doc["calibration"].to<JsonObject>();
  c["calibration_type"] = "baseline_dry";
  c["channel_id"] = channel;
  c["state"] = "completed";
  c["method"] = "commanded";
  c["performed_ts"] = stampMs();
  JsonObject cond = c["conditions"].to<JsonObject>();
  if (haveTemp) cond["coil_surface_temperature_c"] = rnd(tempC, 2);
  JsonObject r = c["result"].to<JsonObject>();
  r["baseline_value"] = rnd(base, 3);
  r["baseline_unit"] = unit;
  r["samples_used"] = s.n;
  r["noise_floor"] = rnd(s.stddev(), 3);
  enqueue("calibration", doc);
}

// ============================================================================ commands (server -> device)
static void captureBaseline(const char* cmdId) {
  Stats a, b;
  for (int i = 0; i < BASELINE_SAMPLES; i++) {      // blocks ~2 s; sampling resumes afterwards
    a.add(readTouchAvg());
    double r, v;
    readResistive(r, v);
    b.add(r);
    delay(50);
  }
  double t, rn;
  bool haveT = readNtc(t, rn);
  cfg.capBase = (float)a.mean;
  cfg.resBase = (float)b.mean;
  cfg.baseTs = stampMs();
  saveConfig();
  sendCalibration("cap-1", "counts", cfg.capBase, a, haveT, t);
  sendCalibration("res-1", "ohm", cfg.resBase, b, haveT, t);
  sendAck(cmdId, "completed");
  resetWindow();                                     // old window was measured against the old baseline
}

static bool applyConfig(JsonObjectConst c, String& err) {
  uint32_t newInterval = cfg.telemetryIntervalS;
  float newRate = cfg.sampleRateHz;

  JsonVariantConst ti = c["reporting"]["telemetry_interval_s"];
  if (!ti.isNull()) {
    if (!ti.is<float>()) { err = "telemetry_interval_s must be a number"; return false; }
    float v = ti.as<float>();
    if (v < MIN_TELEMETRY_INTERVAL_S || v > MAX_TELEMETRY_INTERVAL_S) {
      err = "telemetry_interval_s out of range";
      return false;
    }
    newInterval = (uint32_t)v;
  }
  // v0: all channels share one sample rate; the first channel entry that carries one wins.
  for (JsonObjectConst ch : c["channels"].as<JsonArrayConst>()) {
    JsonVariantConst sr = ch["sample_rate_hz"];
    if (sr.isNull()) continue;
    if (!sr.is<float>()) { err = "sample_rate_hz must be a number"; return false; }
    float v = sr.as<float>();
    if (v < MIN_SAMPLE_RATE_HZ || v > MAX_SAMPLE_RATE_HZ) { err = "sample_rate_hz out of range"; return false; }
    newRate = v;
    break;
  }
  cfg.telemetryIntervalS = newInterval;
  cfg.sampleRateHz = newRate;
  cfg.version = c["config_version"] | (cfg.version + 1);
  saveConfig();
  resetWindow();
  return true;
}

static void handleCommand(const String& raw) {
  JsonDocument d;
  if (deserializeJson(d, raw)) { Serial.println("cmd: invalid JSON"); return; }
  const char* cmdId = d["command"]["command_id"] | "";
  const char* type = d["command"]["command_type"] | "";
  const char* target = d["device_id"] | "";
  if (!*cmdId) { Serial.println("cmd: no command_id"); return; }          // cannot even ack
  if (strcmp(target, deviceId) != 0) { sendAck(cmdId, "rejected", "E_WRONG_DEVICE"); return; }

  uint64_t expires = d["command"]["expires_ts"] | (uint64_t)0;
  if (expires && timeSynced() && epochMs() > expires) { sendAck(cmdId, "expired"); return; }

  Serial.printf("cmd: %s (%s)\n", type, cmdId);
  if (!strcmp(type, "set_config")) {
    String err;
    if (applyConfig(d["command"]["config"].as<JsonObjectConst>(), err)) sendAck(cmdId, "completed", nullptr, nullptr, true);
    else sendAck(cmdId, "rejected", "E_BAD_CONFIG", err.c_str());
  } else if (!strcmp(type, "capture_baseline")) {
    captureBaseline(cmdId);
  } else if (!strcmp(type, "get_status")) {
    sendStatus();
    sendAck(cmdId, "completed");
  } else if (!strcmp(type, "reboot")) {
    sendAck(cmdId, "accepted");
    rebootAt = millis() + 1500;                      // leave time to flush the ack
    if (rebootAt == 0) rebootAt = 1;
  } else {
    sendAck(cmdId, "unsupported", "E_UNSUPPORTED");
  }
}

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  // PubSubClient shares one buffer for in and out: copy now, act later in loop().
  if (cmdInbox.length() == 0) { cmdInbox.concat((const char*)payload, len); }
  else Serial.println("cmd: busy, dropped");
}

// ============================================================================ network
static void connectMqtt() {
  String online = topicFor("online");
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASS : nullptr;
  // Last will: broker publishes "0" (retained) if we vanish without saying goodbye.
  if (!mqtt.connect(deviceId, user, pass, online.c_str(), 1, true, "0")) {
    Serial.printf("mqtt: connect failed, state=%d\n", mqtt.state());
    return;
  }
  reconnects++;
  mqtt.publish(online.c_str(), "1", true);
  mqtt.subscribe(topicFor("cmd").c_str(), 1);
  Serial.println("mqtt: connected");
  if (!bootEventSent) { sendBootEvent(); bootEventSent = true; }
  nextStatusMs = millis();                           // report status right after (re)connect
}

static void serviceNetwork(uint32_t now) {
  static uint32_t lastWifiTry = 0;
  static uint32_t lastMqttTry = (uint32_t)-5000;
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 10000) { lastWifiTry = now; WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
  if (!mqtt.connected()) {
    if (now - lastMqttTry > 5000) { lastMqttTry = now; connectMqtt(); }
    return;
  }
  mqtt.loop();
}

// ============================================================================ arduino entry points
void setup() {
  Serial.begin(115200);
  delay(200);

  prefs.begin("ice", false);
  loadConfig();
  bootCount = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceId, sizeof deviceId, "esp32-%012llx", (unsigned long long)mac);
  Serial.printf("\n%s fw %s boot #%lu\n", deviceId, FW_VERSION, (unsigned long)bootCount);

  pinMode(PIN_RES_EXCITE, OUTPUT);
  digitalWrite(PIN_RES_EXCITE, LOW);
  analogSetAttenuation(ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.google.com");

#ifdef MQTT_USE_TLS
  netClient.setCACert(MQTT_CA_CERT);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  resetWindow();
  nextSampleMs = millis();
  nextStatusMs = millis() + STATUS_INTERVAL_S * 1000UL;
}

void loop() {
  uint32_t now = millis();
  serviceNetwork(now);

  if (cmdInbox.length()) {
    String raw = cmdInbox;
    cmdInbox = "";
    handleCommand(raw);
  }

  if ((int32_t)(now - nextSampleMs) >= 0) {
    sampleOnce();
    uint32_t period = (uint32_t)(1000.0f / cfg.sampleRateHz);
    nextSampleMs += period;
    if ((int32_t)(millis() - nextSampleMs) > (int32_t)period) nextSampleMs = millis();   // fell behind: resync
  }

  if (now - windowStartMs >= cfg.telemetryIntervalS * 1000UL && stCap.n > 0) {
    sendTelemetry();
    resetWindow();
  }

  if ((int32_t)(now - nextStatusMs) >= 0) {
    sendStatus();
    nextStatusMs = now + STATUS_INTERVAL_S * 1000UL;
  }

  flushQueue();

  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) { mqtt.disconnect(); ESP.restart(); }
  delay(2);
}
