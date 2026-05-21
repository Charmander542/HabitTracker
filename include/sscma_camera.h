#pragma once
// =============================================================
// sscma_camera.h — Himax HX6538 (SSCMA) bridge over SPI2
//
// Why SPI and not UART?
//   The SenseCAP Watcher BSP exposes TWO links to the Himax:
//     * BSP_SSCMA_FLASHER_UART_*  (GPIO17/18, 921600 baud) — used ONLY
//       to flash new firmware to the AI chip via X-modem. After the
//       1st-stage bootloader on the Himax hands control to SSCMA, the
//       SSCMA app does not listen on this UART. We confirmed this on
//       hardware: we see the bootloader banner, then silence, even
//       though SSCMA is verifiably alive on USB-CDC (COM4).
//     * BSP_SSCMA_CLIENT_SPI_*    (CS=21, SCLK=4, MOSI=5, MISO=6,
//       SYNC=expander pin 6, 12 MHz, mode 0) — this is the runtime
//       command channel SSCMA actually uses. The Watcher firmware
//       talks to the AI chip via this SPI link, framing each AT/JSON
//       payload in 256-byte SSCMA transport packets.
//
// Packet format (Seeed sscma_client_io_spi.c):
//   buffer[0]   = 0x10                  FEATURE_TRANSPORT
//   buffer[1]   = command code          0x01=READ 0x02=WRITE
//                                        0x03=AVAILABLE 0x06=RESET
//   buffer[2..3]= big-endian uint16     payload length
//   buffer[4..253] = up to 250 bytes payload
//   buffer[4+plen]   = 0xFF             trailer-1
//   buffer[5+plen]   = 0xFF             trailer-2  (positions 254/255
//                                        for a full 250-byte payload)
//   For AVAILABLE/READ requests there's no payload; trailer goes at
//   offsets 4 and 5.
//
// Sync line:
//   The Himax drives SYNC HIGH while it has data ready for us to read.
//   We poll SYNC via the PCA9535 IO expander before issuing AVAILABLE
//   to avoid wasted SPI traffic.
//
// Power / reset:
//   AI chip rail must already be on (BSP_PWR_AI_CHIP, expander pin 11).
//   Reset is expander pin 7 — drive low ~100ms then high to cold-boot
//   the chip. After release the Himax prints its bootloader banner on
//   the FLASHER UART (we don't read that here) and ~150 ms later the
//   SSCMA app starts servicing SPI traffic.
// =============================================================

#include <Arduino.h>
#include <SPI.h>

// Physical pin configuration (from BSP_SSCMA_CLIENT_*).
#define SSCMA_SPI_CS_PIN         21
#define SSCMA_SPI_SCLK_PIN       4
#define SSCMA_SPI_MOSI_PIN       5
#define SSCMA_SPI_MISO_PIN       6
#define SSCMA_SPI_CLOCK_HZ       (12 * 1000 * 1000)

// IO-expander (PCA9535 @ 0x21) pin indices.
#define SSCMA_RST_EXP_PIN        7      // BSP_SSCMA_CLIENT_RST
#define SSCMA_SYNC_EXP_PIN       6      // BSP_SSCMA_CLIENT_SPI_SYNC

// SSCMA transport packet (must match Seeed sscma_client_io_spi.c).
#define SSCMA_HEADER_LEN         4
#define SSCMA_MAX_PL_LEN         250
#define SSCMA_TRAILER_LEN        2
#define SSCMA_PACKET_SIZE        (SSCMA_HEADER_LEN + SSCMA_MAX_PL_LEN + SSCMA_TRAILER_LEN)
#define SSCMA_FEATURE_TRANSPORT  0x10
#define SSCMA_CMD_READ           0x01
#define SSCMA_CMD_WRITE          0x02
#define SSCMA_CMD_AVAILABLE      0x03
#define SSCMA_CMD_RESET          0x06
#define SSCMA_MAX_RECV_SIZE      4095

// SSCMA framing of JSON envelopes:  '\r' '{' ... '}' '\n'
#define SSCMA_FRAME_PREFIX       "\r{"
#define SSCMA_FRAME_SUFFIX       "}\n"

class SscmaCamera {
public:
  // Power rail must be ON (watcher_bsp_begin enables it). begin()
  // pulses RST, opens SPI2, then probes the chip with AT+ID? to
  // confirm we got a JSON response. Returns true on success.
  bool begin(bool verbose = true);

  bool isReady() const { return _ready; }
  bool isAlive() const { return _alive; }

  // Pulse the Himax RST line (expander pin 7). After release the
  // chip needs ~150-300 ms to load SSCMA from flash.
  bool hardReset();

  // Send an AT command (e.g. "AT+ID?\r\n"). Caller must include CRLF.
  bool sendAT(const char* cmd);

  // Wait for the next \r{...}\n envelope (or until timeoutMs). Returns
  // bytes copied into outBuf, or 0 on timeout.
  size_t readJsonResponse(char* outBuf, size_t maxLen, uint32_t timeoutMs,
                          bool debug = false);

  // AT+SENSOR=<id>,<enable>,<opt_id> — must run before SAMPLE for correct
  // resolution (Seeed tf_module_ai_camera.c). opt_id: see config.h
  // SSCMA_SENSOR_OPT_*.
  bool setSensor(uint8_t sensorId, bool enable, int optId);

  // High-level: optional sensor setup, then AT+SAMPLE=1, parse image
  // envelope, base64-decode into jpegOut. Returns true if JPEG magic OK.
  bool captureJpeg(uint8_t* jpegOut, size_t jpegMaxLen, size_t* jpegLen,
                   uint32_t timeoutMs = 6000, bool debug = false);

  // Object-detection path: AT+INVOKE=times,filter,show (Seeed
  // sscma_client_invoke). Then consume SAMPLE/INVOKE events until a JPEG
  // is decoded — same image extraction as captureJpeg().
  bool captureJpegInvoke(uint8_t* jpegOut, size_t jpegMaxLen, size_t* jpegLen,
                         int times = -1, bool filter = false, bool show = true,
                         uint32_t timeoutMs = 12000, bool debug = false);

  // Discard everything in the SSCMA RX buffer.
  void flushRx();

  // Print a one-shot diagnostic dump to Serial.
  void dumpStatus();

  // Send a raw probe and dump any bytes that come back (hex+ascii).
  void blindAt(const char* at);

  // Cold-boot probe: pulse RST, then poll the chip's SPI for any
  // events for `observeMs` and dump them.
  void coldBootProbe(uint32_t observeMs = 4000);

  // Diagnostic: low-level bus check (manually wiggle CS, try
  // AVAILABLE poll, dump SYNC line state).
  void busProbe();

  // Diagnostic: cycle SPI mode 0..3 and try AT+ID? on each, find which
  // one elicits a real SSCMA JSON response.
  void modeSweep();

  // Low-level transport — exposed so the diag commands in main.cpp can
  // poke the bus directly.
  bool   syncLineHigh();          // true if Himax has data ready
  size_t bytesAvailable();        // poll AVAILABLE command, returns N
  bool   readBytes(uint8_t* dst, size_t n);  // request + read N bytes
  bool   writeBytes(const uint8_t* src, size_t n);

private:
  SPIClass _spi = SPIClass(FSPI);    // FSPI = SPI2 on ESP32-S3
  bool     _busInited = false;
  bool     _ready     = false;
  bool     _alive     = false;
  bool     _verbose   = true;
  uint8_t  _spiMode   = SPI_MODE0;
  uint8_t  _txBuf[SSCMA_PACKET_SIZE]; // reuse for outgoing packets
  uint8_t  _rxBuf[SSCMA_PACKET_SIZE]; // reuse for incoming packets

  // Rolling RX scratch buffer for JSON framing.
  String _rxScratch;
  static constexpr size_t SCRATCH_HARD_CAP = 256 * 1024;

  void _log(const char* fmt, ...);

  // SPI helpers
  void _spiTx(const uint8_t* tx, size_t n);
  void _spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n);
  void _spiRxOnly(uint8_t* rx, size_t n);
  void _csLow();
  void _csHigh();

  // SSCMA packet builders
  void _fillHeader(uint8_t cmd, uint16_t len);

  // PCA9535 helpers (own copies; we don't depend on watcher_bsp internals).
  bool _expSetDir(uint8_t pinIdx, bool input);
  bool _expSetLevel(uint8_t pinIdx, bool high);
  bool _expGetLevel(uint8_t pinIdx, bool& outHigh);
};

extern SscmaCamera sscma;
