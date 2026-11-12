#pragma once
#include <cstdint>
#include <string>

struct IPAddress {
  uint8_t bytes[4];
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes{a, b, c, d} {}
};

struct DNSServer {
  bool started = false;
  uint16_t port = 0;
  std::string domain;
  IPAddress address{0, 0, 0, 0};
  uint32_t processCalls = 0;

  bool start(uint16_t p, const char* d, IPAddress ip) {
    port = p;
    domain = d;
    address = ip;
    started = true;
    return true;
  }
  void processNextRequest() { if (started) processCalls++; }
};
