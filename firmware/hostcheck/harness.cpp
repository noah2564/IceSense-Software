// Runs the REAL firmware main.cpp on the PC against fake hardware. A synthetic frost/defrost cycle is
// replayed through simulated I2C sensors; HTTP requests are issued like a browser would; every MQTT message the
// firmware would publish goes to stdout (TOPIC<TAB>PAYLOAD), HTTP exchanges to http.tsv.
#define ICE_HOST_TEST 1
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "main.cpp"

SerialStub Serial; EspStub ESP; WiFiStub WiFi; MDNSStub MDNS; LittleFSStub LittleFS;
uint32_t g_millis = 0;
double g_temperature_c = 0, g_cap_delta_pct = 0, g_trace_log10r = 0;
bool g_fdc_high_address = false, g_broker_up = false;
TwoWire Wire(false), Wire1(true);

struct Row { double temp, cap, log10r; };
static std::vector<Row> trace;
static FILE* httpLog;

static void http(const char* method, const char* path, const char* body = "", bool auth = true) {
  web.request(method, path, body, auth);
  std::string o = web.headers + web.out;
  for (auto& c : o) if (c == '\n' || c == '\t') c = ' ';
  fprintf(httpLog, "%u\t%s\t%s\t%s\t%d\t%s\n", g_millis / 1000, method, path, auth ? "auth" : "noauth", web.code, o.c_str());
}
static void feed(double sec) {
  size_t i = (size_t)(sec * 10); if (i >= trace.size()) i = trace.size() - 1;
  const Row& r = trace[i];
  g_temperature_c = r.temp;
  g_cap_delta_pct = r.cap;
  g_trace_log10r = r.log10r;
}

int main() {
  std::ifstream in("trace.csv"); std::string line;
  while (std::getline(in, line)) { Row r; sscanf(line.c_str(), "%lf,%lf,%lf", &r.temp, &r.cap, &r.log10r); trace.push_back(r); }
  httpLog = fopen("http.tsv", "w");
  g_broker_up = true;
  prefs.putFloat("capbase", 0);  // simulate the persisted "no baseline" sentinel
  setup();
  if (!dnsServer.started || dnsServer.port != 53 || dnsServer.domain != "*" ||
      dnsServer.address.bytes[0] != 192 || dnsServer.address.bytes[1] != 168 ||
      dnsServer.address.bytes[2] != 4 || dnsServer.address.bytes[3] != 1) {
    fprintf(stderr, "DNS responder did not start with the captive-portal address\n");
    return 1;
  }
  http("GET", "/generate_204", "", false);
  http("GET", "/hotspot-detect.html", "", false);
  http("GET", "/connecttest.txt", "", false);
  http("GET", "/redirect", "", false);
  sendStatus();
  uint32_t nextSensor = 0; bool did[24] = {};
  auto at = [&](int s, int k) { if (g_millis / 1000 >= (uint32_t)s && !did[k]) { did[k] = true; return true; } return false; };
  while (g_millis < 640000) {
    feed(g_millis / 1000.0);
    if (g_millis >= nextSensor) { sensorTick(); nextSensor += 1000 / SAMPLE_RATE_HZ; }
    if (at(1, 0))  http("POST", "/api/baseline");                                  // too early: not enough samples
    if (at(5, 1))  http("POST", "/api/baseline");
    if (at(6, 2))  http("GET", "/api/state", "", false);                           // no credentials -> 401
    if (at(7, 3))  http("GET", "/");
    if (at(8, 4))  http("POST", "/api/params", R"({"ice_pct":25})");
    if (at(9, 5))  http("POST", "/api/params", R"({"ice_pct":"x"})");
    if (at(9, 6))  http("POST", "/api/params", R"({"nope":1})");
    if (at(9, 7))  http("POST", "/api/params", R"({"defrost_off_mm":5,"defrost_on_mm":2})");
    if (at(10, 8)) http("POST", "/api/params", R"({"ice_pct":20})");
    if (at(11, 9)) http("POST", "/api/relay", R"({"enabled":"yes"})");
    if (at(12, 10)) http("GET", "/api/state");
    if (at(100, 11)) http("POST", "/api/relay", R"({"enabled":true})");           // cycle 1 ran with the relay disabled
    if (at(60, 16))  http("GET", "/api/state");                                     // clear period, relay disabled
    if (at(450, 15)) http("GET", "/api/state");                                    // ice, relay enabled
    if (at(300, 12)) http("GET", "/api/state");                                    // ice period
    if (at(630, 13)) { http("GET", "/api/state"); http("GET", "/api/history.csv"); }
    if (at(631, 14)) {
#if MQTT_ENABLED
      char buf[300];
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"c1","command_type":"set_config","config":{"algorithm":{"parameters":{"ice_pct":22,"confirm_windows":3}}}}})", deviceId); mqtt.inject(buf);
#endif
    }
    if (at(633, 17)) {
#if MQTT_ENABLED
      char buf[300];
      snprintf(buf, sizeof buf, R"({"device_id":"%s","command":{"command_id":"c2","command_type":"set_output","params":{"output":"relay","enabled":false}}})", deviceId); mqtt.inject(buf);
#endif
    }
    g_millis += 10;
    loop();
  }
  if (!dnsServer.processCalls) {
    fprintf(stderr, "DNS responder was not serviced by the main loop\n");
    return 1;
  }
  fclose(httpLog);
  return 0;
}
