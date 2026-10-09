#pragma once
#include <Arduino.h>
enum wifi_auth_mode_t {
  WIFI_AUTH_OPEN,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_WPA2_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK
};
constexpr int WIFI_OFF = 0, WIFI_STA = 1, WL_CONNECTED = 3;
struct FakeIP {
  String toString() const { return String("127.0.0.1"); }
};
struct FakeWiFi {
  int connection_status = WL_CONNECTED;
  FakeIP localIP() const { return {}; }
  String macAddress() const { return String("00:00:00:00:00:01"); }
  int RSSI(int = 0) const { return -40; }
  void disconnect(bool = false, bool = false) {}
  void mode(int) {}
  void persistent(bool) {}
  void setSleep(bool) {}
  void setAutoReconnect(bool) {}
  void begin(const char *, const char *) {}
  int status() const { return connection_status; }
  int scanNetworks(bool, bool) const { return 0; }
  String SSID(int) const { return String("test"); }
  wifi_auth_mode_t encryptionType(int) const { return WIFI_AUTH_WPA2_PSK; }
  int channel(int) const { return 1; }
  void scanDelete() {}
};
inline FakeWiFi WiFi;
