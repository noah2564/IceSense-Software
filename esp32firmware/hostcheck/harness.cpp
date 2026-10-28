// Runs the REAL firmware main.cpp against fake hardware and prints what it would publish.
#include <cstdlib>
#include <string>
#include "main.cpp"      // pulled in via -I firmware/src

SerialStub Serial; EspStub ESP; WiFiStub WiFi;
uint32_t g_millis = 0; double g_ntc_mv = 1900, g_res_mv = 1200, g_touch = 50000; bool g_broker_up = false;

static void cmd(const char* json) { mqtt.inject(json); }

int main() {
  setup();
  uint32_t lastSecond = 0;
  bool did[8] = {};
  while (g_millis < 70000) {
    uint32_t s = g_millis / 1000;
    if (s != lastSecond) {
      lastSecond = s;
      g_touch = (s < 30 ? 50000 : 56500) + (rand() % 40);          // capacitance rises after baseline
    }
    g_broker_up = g_millis >= 12000;                                // broker down for the first 12 s -> buffering
    uint64_t now = epochMs(), exp = now + 60000;
    char buf[600];
    if (s >= 25 && !did[0]) { did[0] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-base","command_type":"capture_baseline"}})", deviceId); cmd(buf); }
    if (s >= 32 && !did[1]) { did[1] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-cfg","command_type":"set_config","config":{"config_version":5,"reporting":{"telemetry_interval_s":5},"channels":[{"channel_id":"cap-1","sample_rate_hz":5}]}}})", deviceId); cmd(buf); }
    if (s >= 40 && !did[2]) { did[2] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-bad","command_type":"set_config","config":{"reporting":{"telemetry_interval_s":1}}}})", deviceId); cmd(buf); }
    if (s >= 45 && !did[3]) { did[3] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-st","command_type":"get_status"}})", deviceId); cmd(buf); }
    if (s >= 50 && !did[4]) { did[4] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-x","command_type":"start_ota"}})", deviceId); cmd(buf); }
    if (s >= 52 && !did[5]) { did[5] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"esp32-someoneelse","command":{"command_id":"cmd-wrong","command_type":"get_status"}})"); cmd(buf); }
    if (s >= 54 && !did[6]) { did[6] = true;
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"cmd-old","command_type":"get_status","expires_ts":1000}})", deviceId); cmd(buf); }
    if (s >= 58 && !did[7]) { did[7] = true; g_ntc_mv = 10; }       // NTC wire breaks: temperature channel must go bad
    (void)exp;
    loop();
  }
  return 0;
}
