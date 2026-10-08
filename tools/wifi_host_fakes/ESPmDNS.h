#pragma once
#include <Arduino.h>
struct FakeMDNS {
  bool begin(const char *) { return true; }
  void addService(const char *, const char *, unsigned) {}
  template <typename T>
  void addServiceTxt(const char *, const char *, const char *, const T &) {}
  void end() {}
};
inline FakeMDNS MDNS;
