#pragma once
struct MDNSStub { bool begin(const char*) { return true; } };
extern MDNSStub MDNS;
