#pragma once
// =============================================================
// storage.h — LittleFS read/write helpers
//
// Wraps all LittleFS operations with error handling and logging.
// Provides JSON convenience wrappers and capture file management
// (rolling delete to stay within MAX_CAPTURES).
// =============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

class Storage {
public:
  // Mount LittleFS. Returns true on success.
  // If mount fails, call format() then begin() again.
  bool begin();

  // Format the filesystem. Destroys all data. Use as last resort.
  bool format();

  // ---- JSON helpers ----

  // Write a JsonDocument to a file path. Creates parent dirs if needed.
  // Returns true on success.
  bool writeJSON(const String& path, const JsonDocument& doc);

  // Read a file and deserialise it into doc.
  // Returns true on success. Logs error to Serial on failure.
  bool readJSON(const String& path, JsonDocument& doc);

  // ---- Raw file helpers ----

  // Write raw bytes (e.g. JPEG camera output) to a path.
  // Returns true on success.
  bool writeBytes(const String& path, const uint8_t* data, size_t len);

  // Read a file into a String. Returns "" on failure.
  String readString(const String& path);

  // Delete a file. Returns true if deleted or didn't exist.
  bool remove(const String& path);

  // ---- Directory helpers ----

  // Create a directory and any missing parents. Returns true on success.
  bool ensureDir(const String& path);

  // ---- Capture management ----

  // Save a JPEG camera capture to /captures/YYYY-MM-DD_habitName.jpg.
  // Automatically prunes old captures if count > MAX_CAPTURES.
  // Returns the path written, or "" on failure.
  String saveCapture(const String& date, const String& habitName,
                     const uint8_t* jpegData, size_t jpegLen);

  // Delete oldest captures until count <= keepCount.
  void pruneCaptures(int keepCount = MAX_CAPTURES);

  // ---- Space info ----

  size_t totalBytes() const;
  size_t usedBytes()  const;
  size_t freeBytes()  const;

private:
  bool _mounted = false;
};
