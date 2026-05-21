// =============================================================
// storage.cpp — LittleFS filesystem operations
//
// All file operations log failures to Serial rather than panicking.
// The camera capture manager enforces a rolling MAX_CAPTURES limit
// by sorting by name (which is date-prefixed) and deleting the oldest.
// =============================================================

#include "storage.h"
#include "config.h"
#include <Arduino.h>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------
// begin — mount LittleFS
// ---------------------------------------------------------------
bool Storage::begin() {
  // `true` = format on failure (one-time recovery; subsequent boots will mount cleanly)
  if (!LittleFS.begin(true)) {
    Serial.println("[Storage] ERROR: LittleFS mount failed");
    _mounted = false;
    return false;
  }
  _mounted = true;

  // Ensure required directories exist on first boot
  ensureDir(PATH_CAPTURES_DIR);
  ensureDir(PATH_LOGS_DIR);

  Serial.printf("[Storage] Mounted. Used: %u / %u bytes\n",
                LittleFS.usedBytes(), LittleFS.totalBytes());
  return true;
}

// ---------------------------------------------------------------
// format — erase and re-initialise the filesystem
// ---------------------------------------------------------------
bool Storage::format() {
  Serial.println("[Storage] Formatting LittleFS...");
  if (!LittleFS.format()) {
    Serial.println("[Storage] ERROR: Format failed");
    return false;
  }
  Serial.println("[Storage] Format complete");
  return true;
}

// ---------------------------------------------------------------
// writeJSON — serialise JsonDocument to a file
// ---------------------------------------------------------------
bool Storage::writeJSON(const String& path, const JsonDocument& doc) {
  if (!_mounted) { Serial.println("[Storage] Not mounted"); return false; }

  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[Storage] ERROR: Cannot open %s for write\n", path.c_str());
    return false;
  }

  size_t written = serializeJson(doc, f);
  f.close();

  if (written == 0) {
    Serial.printf("[Storage] ERROR: serializeJson wrote 0 bytes to %s\n", path.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------
// readJSON — deserialise a file into a JsonDocument
// ---------------------------------------------------------------
bool Storage::readJSON(const String& path, JsonDocument& doc) {
  if (!_mounted) { Serial.println("[Storage] Not mounted"); return false; }

  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[Storage] File not found: %s\n", path.c_str());
    return false;
  }

  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[Storage] ERROR: deserializeJson(%s): %s\n",
                  path.c_str(), err.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------
// writeBytes — write raw byte buffer to a file path
// ---------------------------------------------------------------
bool Storage::writeBytes(const String& path, const uint8_t* data, size_t len) {
  if (!_mounted) return false;

  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[Storage] ERROR: Cannot open %s for write\n", path.c_str());
    return false;
  }

  size_t written = f.write(data, len);
  f.close();

  if (written != len) {
    Serial.printf("[Storage] ERROR: Wrote %u of %u bytes to %s\n",
                  written, len, path.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------
// readString — read entire file into a String
// ---------------------------------------------------------------
String Storage::readString(const String& path) {
  if (!_mounted) return "";

  File f = LittleFS.open(path, "r");
  if (!f) return "";

  String result = f.readString();
  f.close();
  return result;
}

// ---------------------------------------------------------------
// remove — delete a file
// ---------------------------------------------------------------
bool Storage::remove(const String& path) {
  if (!_mounted) return false;
  return LittleFS.remove(path);
}

// ---------------------------------------------------------------
// ensureDir — create directory (and parents) if missing
// ---------------------------------------------------------------
bool Storage::ensureDir(const String& path) {
  if (!_mounted) return false;

  if (LittleFS.exists(path)) return true;

  if (!LittleFS.mkdir(path)) {
    Serial.printf("[Storage] ERROR: mkdir(%s) failed\n", path.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------
// saveCapture — write JPEG and prune old captures
// ---------------------------------------------------------------
String Storage::saveCapture(const String& date, const String& habitName,
                             const uint8_t* jpegData, size_t jpegLen) {
  if (!_mounted || !jpegData || jpegLen == 0) return "";

  ensureDir(PATH_CAPTURES_DIR);

  // Sanitise habit name: replace spaces with underscores
  String safeName = habitName;
  safeName.replace(" ", "_");

  String path = String(PATH_CAPTURES_DIR) + "/" + date + "_" + safeName + ".jpg";

  if (!writeBytes(path, jpegData, jpegLen)) return "";

  Serial.printf("[Storage] Capture saved: %s (%u bytes)\n", path.c_str(), jpegLen);

  // Keep storage tidy
  pruneCaptures(MAX_CAPTURES);
  return path;
}

// ---------------------------------------------------------------
// pruneCaptures — delete oldest captures when over the limit
//
// Files are named YYYY-MM-DD_*, so alphabetical sort = date order.
// We collect all names, sort them, then delete from the front.
// ---------------------------------------------------------------
void Storage::pruneCaptures(int keepCount) {
  if (!_mounted) return;

  File dir = LittleFS.open(PATH_CAPTURES_DIR);
  if (!dir || !dir.isDirectory()) return;

  // Collect all file names
  std::vector<String> names;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      names.push_back(String(entry.name()));
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if ((int)names.size() <= keepCount) return;   // Nothing to prune

  // Sort alphabetically (date-prefix ensures chronological order)
  std::sort(names.begin(), names.end());

  int deleteCount = (int)names.size() - keepCount;
  for (int i = 0; i < deleteCount; i++) {
    String fullPath = String(PATH_CAPTURES_DIR) + "/" + names[i];
    LittleFS.remove(fullPath);
    Serial.printf("[Storage] Pruned capture: %s\n", fullPath.c_str());
  }
}

// ---------------------------------------------------------------
// Space info helpers
// ---------------------------------------------------------------
size_t Storage::totalBytes() const { return _mounted ? LittleFS.totalBytes() : 0; }
size_t Storage::usedBytes()  const { return _mounted ? LittleFS.usedBytes()  : 0; }
size_t Storage::freeBytes()  const { return totalBytes() - usedBytes(); }
