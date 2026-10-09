#pragma once
#include <sys/time.h>
inline void (*fake_time_callback)(struct timeval *) = nullptr;
inline unsigned fake_ntp_starts = 0;
inline unsigned fake_ntp_stops = 0;
inline void
esp_sntp_set_time_sync_notification_cb(void (*callback)(struct timeval *)) {
  fake_time_callback = callback;
}
inline void esp_sntp_stop() { ++fake_ntp_stops; }
inline void configTime(long, int, const char *, const char *) {
  ++fake_ntp_starts;
}
