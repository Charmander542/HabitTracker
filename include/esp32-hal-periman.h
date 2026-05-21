#pragma once
// =============================================================
// Shim for Arduino-ESP32 core < 3.1
//
// Arduino_GFX 1.6+ includes esp32-hal-periman.h from Arduino_ESP32SPI.h
// even when the project only uses Arduino_ESP32QSPI. The official header
// ships with Arduino-ESP32 3.1+. This empty placeholder satisfies the
// include on PlatformIO espressif32@6.x (Arduino core 2.x).
//
// When you migrate to Arduino-ESP32 3.1+, delete this file so the real
// peripheral manager API is used.
// =============================================================
