// =============================================================
// rtc.cpp — ESP32 system clock with optional NTP synchronisation
//
// The ESP32's internal RTC keeps time across soft reboots. On a
// cold start (power loss) the clock resets to epoch 0 (Jan 1 1970).
// syncNTP() corrects this when WiFi is available.
//
// configTime() from the ESP32 Arduino core initialises SNTP and
// sets the system clock. All subsequent calls to getLocalTime()
// return properly zone-adjusted local time.
// =============================================================

#include "rtc.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// ---------------------------------------------------------------
// begin — apply timezone and kick off SNTP daemon
// ---------------------------------------------------------------
void RTC::begin() {
  // configTime sets the POSIX timezone offset and starts the SNTP daemon.
  // If WiFi isn't connected yet, SNTP will retry in the background.
  configTime(TZ_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  Serial.printf("[RTC] Configured TZ offset %d sec, DST %d sec\n",
                TZ_OFFSET_SEC, DST_OFFSET_SEC);

  // Give SNTP up to 500 ms to sync if WiFi is already connected
  if (WiFi.isConnected()) {
    unsigned long deadline = millis() + 500;
    struct tm ti;
    while (!getLocalTime(&ti, 0) && millis() < deadline) {
      delay(10);
    }
    if (getLocalTime(&ti, 0)) {
      _ntpSynced = true;
      Serial.printf("[RTC] NTP synced: %04d-%02d-%02d %02d:%02d\n",
                    ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                    ti.tm_hour, ti.tm_min);
    } else {
      Serial.println("[RTC] NTP not available yet (no WiFi or timeout)");
    }
  }
}

// ---------------------------------------------------------------
// syncNTP — attempt NTP sync; returns true if time is valid
// ---------------------------------------------------------------
bool RTC::syncNTP() {
  if (!WiFi.isConnected()) return false;

  struct tm ti;
  // Allow up to 5 seconds for a full NTP exchange
  if (!getLocalTime(&ti, 5000)) {
    Serial.println("[RTC] NTP sync timed out");
    return false;
  }

  _ntpSynced = true;
  Serial.printf("[RTC] NTP synced: %04d-%02d-%02d %02d:%02d\n",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min);
  return true;
}

// ---------------------------------------------------------------
// setTime — manually set the system clock (serial config menu)
// ---------------------------------------------------------------
void RTC::setTime(time_t epochSec) {
  struct timeval tv = { .tv_sec = epochSec, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[RTC] Time set manually to epoch %lld\n", (long long)epochSec);
}

// ---------------------------------------------------------------
// getDate — returns "YYYY-MM-DD"
// ---------------------------------------------------------------
String RTC::getDate() {
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return "1970-01-01";

  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
  return String(buf);
}

// ---------------------------------------------------------------
// getTimeStr — returns "HH:MM" (24-hour)
// ---------------------------------------------------------------
String RTC::getTimeStr() {
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return "00:00";

  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
  return String(buf);
}

// ---------------------------------------------------------------
// getHour — current local hour 0–23
// ---------------------------------------------------------------
int RTC::getHour() {
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return 0;
  return ti.tm_hour;
}

// ---------------------------------------------------------------
// getMinute — current local minute 0–59
// ---------------------------------------------------------------
int RTC::getMinute() {
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return 0;
  return ti.tm_min;
}

// ---------------------------------------------------------------
// getTimestamp — raw Unix timestamp in seconds
// ---------------------------------------------------------------
time_t RTC::getTimestamp() {
  return time(nullptr);
}

// ---------------------------------------------------------------
// isNewDay — returns true if today's date differs from lastDate
// ---------------------------------------------------------------
bool RTC::isNewDay(const String& lastDate) {
  return getDate() != lastDate;
}

// ---------------------------------------------------------------
// getLocalTm — fill a tm struct with current local time
// ---------------------------------------------------------------
bool RTC::getLocalTm(struct tm* out) {
  return getLocalTime(out, 100);
}

// ---------------------------------------------------------------
// dateAddDays — shift an ISO calendar date by deltaDays (local TZ)
// ---------------------------------------------------------------
String RTC::dateAddDays(const String& isoYmd, int deltaDays) {
  int y = 0, m = 0, d = 0;
  if (sscanf(isoYmd.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return "";
  if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return "";

  struct tm ti = {};
  ti.tm_year  = y - 1900;
  ti.tm_mon   = m - 1;
  ti.tm_mday  = d + deltaDays;
  ti.tm_hour  = 12;   // noon avoids most DST fold/ambiguous edges
  ti.tm_isdst = -1;

  time_t t = mktime(&ti);
  if (t == (time_t)-1) return "";

  struct tm out;
  localtime_r(&t, &out);

  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           out.tm_year + 1900, out.tm_mon + 1, out.tm_mday);
  return String(buf);
}

// ---------------------------------------------------------------
// compareIsoDate — lexicographic compare (valid for ISO YYYY-MM-DD)
// ---------------------------------------------------------------
int RTC::compareIsoDate(const String& a, const String& b) {
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}
