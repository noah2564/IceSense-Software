// Host stand-in for the Arduino WebServer: routes are registered as on the device and the harness
// calls request() to run a handler, capturing the response.
#pragma once
#include <functional>
#include <map>
#include "Arduino.h"
#include "LittleFS.h"
#define HTTP_GET 1
#define HTTP_POST 3
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
struct WebServer {
  std::map<std::string, std::function<void()>> routes;
  std::function<void()> notFound;
  std::string body_, user_; bool authOk_ = true;
  int code = 0; std::string ctype, out, headers;
  WebServer(int) {}
  void on(const char* uri, int method, std::function<void()> fn) { routes[std::string(method == HTTP_GET ? "GET " : "POST ") + uri] = fn; }
  void onNotFound(std::function<void()> fn) { notFound = fn; }
  void begin() {}
  void handleClient() {}
  bool authenticate(const char*, const char*) { return authOk_; }
  void requestAuthentication() { code = 401; out = "auth"; }
  String arg(const char*) { return String(body_.c_str()); }
  void sendHeader(const char* k, const char* v) { headers += std::string(k) + ": " + v + "\n"; }
  void send(int c, const char* t, const String& b) { if (c) code = c; ctype = t; out += b.s; }
  void send(int c, const char* t, const char* b) { if (c) code = c; ctype = t; out += b; }
  void setContentLength(size_t) {}
  void sendContent(const String& b) { out += b.s; }
  void sendContent(const char* b) { out += b; }
  void streamFile(File&, const char* t) { code = 200; ctype = t; out += "<gz-page>"; }
  // harness side
  void request(const char* method, const char* path, const char* body, bool auth) {
    code = 0; out.clear(); headers.clear(); ctype.clear(); body_ = body; authOk_ = auth;
    auto it = routes.find(std::string(method) + " " + path);
    if (it == routes.end()) notFound(); else it->second();
    if (!code) code = 200;
  }
};
