#pragma once
// =============================================================
// rtc.h — Time and date tracking via ESP32 RTC + optional NTP
//
// On first boot (or after long power-off) the ESP32 RTC starts
// at epoch 0. syncNTP() corrects this if WiFi is available.
// The device can function without WiFi — users can set time via
// the serial config menu if NTP is unavailable.
// =============================================================

#include <Arduino.h>
#include <time.h>

class RTC {
public:
  // Configure system time from SNTP; call in setup().
  // If WiFi is not connected, falls back to the internal RTC.
  void begin();

  // Attempt NTP sync over WiFi. Returns true if time was set.
  // Non-blocking after the SNTP daemon is running (uses poll).
  bool syncNTP();

  // Set time manually (for serial config menu or testing).
  // `epochSec` = Unix timestamp in seconds.
  void setTime(time_t epochSec);

  // Returns "YYYY-MM-DD" for today (local time)
  String getDate();

  // Returns "HH:MM" for current local time (24-hour)
  String getTimeStr();

  // Returns current local hour (0–23)
  int getHour();

  // Returns current local minute (0–59)
  int getMinute();

  // Returns the raw Unix timestamp in seconds
  time_t getTimestamp();

  // Returns true if the current date is different from `lastDate`.
  // Used in main.cpp to trigger the day-rollover logic.
  bool isNewDay(const String& lastDate);

  // ISO "YYYY-MM-DD" calendar math (local timezone). Returns "" on bad input.
  static String dateAddDays(const String& isoYmd, int deltaDays);
  static int    compareIsoDate(const String& a, const String& b);

  // Fill a tm struct with current local time. Returns true on success.
  bool getLocalTm(struct tm* out);

private:
  bool _ntpSynced = false;
};
