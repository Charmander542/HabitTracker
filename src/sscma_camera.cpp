// =============================================================
// sscma_camera.cpp — SPI bridge to the Himax HX6538 SSCMA AI chip
//
// See sscma_camera.h for the full theory of operation. Short version:
//
//   1. Watcher BSP has already powered the AI chip rail (expander
//      pin 11) and the system rails. We only need to pulse RST.
//   2. Open SPI2 with manual CS so we can drive multi-step packet
//      transactions. Mode 0, MSB-first, 12 MHz.
//   3. To send: pack into 256-byte WRITE packets, send each one,
//      sleep 10ms between.
//   4. To receive: poll SYNC line. When HIGH, send an AVAILABLE
//      packet, read 2 bytes => length N. If N > 0, send a READ
//      packet, then read N bytes. The Himax frames each AT
//      response as exactly "\r{...}\n" so we scan a rolling
//      scratch buffer for those envelopes.
//   5. AT+SAMPLE=1 → ACK envelope, then a data envelope whose
//      "data.image" field is base64-encoded JPEG. We decode that
//      directly into the caller's buffer.
//
// All pin numbers come from the Watcher BSP and are documented in
// sscma_camera.h.
// =============================================================

#include "sscma_camera.h"
#include <Wire.h>
#include "config.h"

static const uint8_t PCA9535_IN_PORT  = 0x00;
static const uint8_t PCA9535_OUT_PORT = 0x02;
static const uint8_t PCA9535_CFG_PORT = 0x06;
static const uint32_t SSCMA_WAIT_MS   = 10;   // mandatory inter-packet delay

SscmaCamera sscma;

// ------------------------------------------------------------------
// PCA9535 helpers + shadows so we can flip individual bits without
// disturbing rails set by watcher_bsp_begin().
// ------------------------------------------------------------------
static uint16_t s_expOutShadow = 0;
static uint16_t s_expCfgShadow = 0xFFFF;
static bool     s_expShadowValid = false;

static bool pca9535_write16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value & 0xFF));
  Wire.write((uint8_t)(value >> 8));
  return Wire.endTransmission() == 0;
}

static bool pca9535_read16(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WATCHER_IOEXP_ADDR, 2) != 2) return false;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  *out = (uint16_t)lo | ((uint16_t)hi << 8);
  return true;
}

static bool pca9535_refreshShadows() {
  if (!pca9535_read16(PCA9535_OUT_PORT, &s_expOutShadow)) return false;
  if (!pca9535_read16(PCA9535_CFG_PORT, &s_expCfgShadow)) return false;
  s_expShadowValid = true;
  return true;
}

bool SscmaCamera::_expSetDir(uint8_t pinIdx, bool input) {
  if (!s_expShadowValid && !pca9535_refreshShadows()) return false;
  uint16_t mask = (uint16_t)(1u << pinIdx);
  uint16_t next = input ? (s_expCfgShadow | mask) : (s_expCfgShadow & ~mask);
  if (next == s_expCfgShadow) return true;
  if (!pca9535_write16(PCA9535_CFG_PORT, next)) return false;
  s_expCfgShadow = next;
  return true;
}

bool SscmaCamera::_expSetLevel(uint8_t pinIdx, bool high) {
  if (!s_expShadowValid && !pca9535_refreshShadows()) return false;
  uint16_t mask = (uint16_t)(1u << pinIdx);
  uint16_t next = high ? (s_expOutShadow | mask) : (s_expOutShadow & ~mask);
  if (next == s_expOutShadow) return true;
  if (!pca9535_write16(PCA9535_OUT_PORT, next)) return false;
  s_expOutShadow = next;
  return true;
}

bool SscmaCamera::_expGetLevel(uint8_t pinIdx, bool& outHigh) {
  uint16_t in = 0;
  if (!pca9535_read16(PCA9535_IN_PORT, &in)) return false;
  outHigh = (in & (1u << pinIdx)) != 0;
  return true;
}

void SscmaCamera::_log(const char* fmt, ...) {
  if (!_verbose) return;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(F("[sscma] "));
  Serial.println(buf);
}

// ------------------------------------------------------------------
// Low-level SPI helpers. We do manual CS control so we can sequence
// multi-stage transactions (WRITE -> wait -> AVAILABLE -> read 2 -> ...)
// ------------------------------------------------------------------
void SscmaCamera::_csLow()  { digitalWrite(SSCMA_SPI_CS_PIN, LOW); }
void SscmaCamera::_csHigh() { digitalWrite(SSCMA_SPI_CS_PIN, HIGH); }

void SscmaCamera::_spiTx(const uint8_t* tx, size_t n) {
  _spi.beginTransaction(SPISettings(SSCMA_SPI_CLOCK_HZ, MSBFIRST, _spiMode));
  _csLow();
  _spi.transferBytes(tx, nullptr, n);
  _csHigh();
  _spi.endTransaction();
}

void SscmaCamera::_spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) {
  _spi.beginTransaction(SPISettings(SSCMA_SPI_CLOCK_HZ, MSBFIRST, _spiMode));
  _csLow();
  _spi.transferBytes(tx, rx, n);
  _csHigh();
  _spi.endTransaction();
}

void SscmaCamera::_spiRxOnly(uint8_t* rx, size_t n) {
  static uint8_t zero[64] = {0};
  // ESP32 SPI requires a TX buffer; send zeros while clocking N bytes in.
  _spi.beginTransaction(SPISettings(SSCMA_SPI_CLOCK_HZ, MSBFIRST, _spiMode));
  _csLow();
  size_t off = 0;
  while (off < n) {
    size_t chunk = n - off;
    if (chunk > sizeof(zero)) chunk = sizeof(zero);
    _spi.transferBytes(zero, rx + off, chunk);
    off += chunk;
  }
  _csHigh();
  _spi.endTransaction();
}

void SscmaCamera::_fillHeader(uint8_t cmd, uint16_t len) {
  // CRITICAL: trailer position depends on whether this packet carries a
  // payload. Got this wrong on the first pass and it cost us a flashing
  // session worth of debugging — `_fillHeader(CMD_READ, 4095)` was writing
  // off the end of the 256-byte _txBuf and corrupting _rxBuf / _rxScratch.
  //
  //   * WRITE  : payload follows at [4..3+plen], trailer at [4+plen][5+plen].
  //              Caller (writeBytes) does the memcpy + trailer placement.
  //   * READ   : 'len' field tells the chip how many bytes WE want back.
  //              No payload — trailer always at [4][5].
  //   * AVAILABLE / RESET : same as READ, no payload, trailer at [4][5].
  memset(_txBuf, 0, SSCMA_PACKET_SIZE);
  _txBuf[0] = SSCMA_FEATURE_TRANSPORT;
  _txBuf[1] = cmd;
  _txBuf[2] = (uint8_t)(len >> 8);
  _txBuf[3] = (uint8_t)(len & 0xFF);
  if (cmd != SSCMA_CMD_WRITE) {
    _txBuf[4] = 0xFF;
    _txBuf[5] = 0xFF;
  }
  // For WRITE the caller fills [4..3+plen] with payload then sets the
  // trailer at [4+plen][5+plen] explicitly.
}

// ==================================================================
//  PUBLIC
// ==================================================================

bool SscmaCamera::begin(bool verbose) {
  _verbose = verbose;
  _ready = false;
  _alive = false;
  _rxScratch = "";

  _log("begin(): SPI2 SCLK=%d MOSI=%d MISO=%d CS=%d clk=%dHz mode=0",
       SSCMA_SPI_SCLK_PIN, SSCMA_SPI_MOSI_PIN, SSCMA_SPI_MISO_PIN,
       SSCMA_SPI_CS_PIN, SSCMA_SPI_CLOCK_HZ);

  if (!pca9535_refreshShadows()) {
    _log("ERR: PCA9535 shadow refresh failed (is watcher_bsp initialised?)");
    return false;
  }
  _log("expander cfg=0x%04X out=0x%04X", s_expCfgShadow, s_expOutShadow);

  if ((s_expOutShadow & BSP_PWR_AI_CHIP) == 0) {
    _log("AI chip rail is OFF — turning ON");
    if (!_expSetLevel(11, true)) {
      _log("ERR: failed to assert AI power rail");
      return false;
    }
    delay(200);
  }

  // SYNC = expander input (Himax drives it)
  _expSetDir(SSCMA_SYNC_EXP_PIN, /*input=*/true);
  // RST = expander output, default HIGH (active-low reset)
  _expSetDir(SSCMA_RST_EXP_PIN, /*input=*/false);
  _expSetLevel(SSCMA_RST_EXP_PIN, true);

  // CS pin as plain GPIO output (we drive it manually)
  pinMode(SSCMA_SPI_CS_PIN, OUTPUT);
  digitalWrite(SSCMA_SPI_CS_PIN, HIGH);

  if (!_busInited) {
    _spi.begin(SSCMA_SPI_SCLK_PIN, SSCMA_SPI_MISO_PIN,
               SSCMA_SPI_MOSI_PIN, -1);   // -1 == manual CS
    _busInited = true;
  }

  _ready = true;

  // Cold-reset the chip and wait for SSCMA to load.
  hardReset();
  // After RST release: bootloader prints banner on UART, then ~150-300 ms
  // later SSCMA starts replying to SPI. We wait conservatively.
  delay(800);

  // First ping: drain anything stale, then fire AT+ID? and look for
  // a JSON envelope.
  flushRx();

  Serial.println(F("[sscma] probing SPI link with AT+ID? ..."));
  if (!sendAT("AT+ID?\r\n")) {
    Serial.println(F("[sscma] ERR: AT+ID? write failed"));
    return false;
  }

  char resp[512];
  size_t got = readJsonResponse(resp, sizeof(resp), 3000, /*debug=*/verbose);
  if (got == 0) {
    Serial.println(F("[sscma] no JSON response to AT+ID? within 3s"));
    Serial.print(F("[sscma]   scratch length: "));
    Serial.println(_rxScratch.length());
    if (_rxScratch.length() > 0) {
      Serial.print(F("[sscma]   scratch tail (hex): "));
      size_t L = _rxScratch.length();
      size_t S = L < 32 ? L : 32;
      for (size_t i = L - S; i < L; i++) {
        Serial.printf("%02X ", (uint8_t)_rxScratch.charAt(i));
      }
      Serial.println();
    }
    Serial.println(F("[sscma]   try `cambus`, `camsweep`, `camcold 4000`"));
    return false;
  }
  _log("AT+ID? -> %u bytes", (unsigned)got);
  Serial.print(F("[sscma] response: "));
  Serial.write((uint8_t*)resp, got);
  Serial.println();

  // Identify the chip a bit more (best-effort).
  if (sendAT("AT+NAME?\r\n")) {
    size_t n = readJsonResponse(resp, sizeof(resp), 1500, false);
    if (n) { Serial.print(F("[sscma]   NAME: ")); Serial.println(resp); }
  }
  if (sendAT("AT+VER?\r\n")) {
    size_t n = readJsonResponse(resp, sizeof(resp), 1500, false);
    if (n) { Serial.print(F("[sscma]   VER:  ")); Serial.println(resp); }
  }

  _alive = true;
  Serial.println(F("[sscma] *** Himax SSCMA SPI link ESTABLISHED — "
                   "camera ready ***"));
  return true;
}

bool SscmaCamera::hardReset() {
  if (!_ready && !_busInited) {
    // begin() needs to call us before the bus is up; that's fine.
  }
  _log("hardReset(): RST low 100ms, high");
  _expSetLevel(SSCMA_RST_EXP_PIN, false);
  delay(100);
  _expSetLevel(SSCMA_RST_EXP_PIN, true);
  delay(50);
  return true;
}

// ------------------------------------------------------------------
// Sync line — Himax drives it HIGH while it has buffered events ready
// for us to read. If LOW, AVAILABLE will return 0 (we skip it).
// ------------------------------------------------------------------
bool SscmaCamera::syncLineHigh() {
  bool h = false;
  if (!_expGetLevel(SSCMA_SYNC_EXP_PIN, h)) return false;
  return h;
}

// ------------------------------------------------------------------
// Send AVAILABLE poll, return how many bytes the chip has buffered.
// Mirrors client_io_spi_available() in Seeed sscma_client.
//
// Sequence (matches Seeed):
//   * skip the whole thing if SYNC line is low (chip has nothing)
//   * delay 10 ms (let the chip settle)
//   * transmit 256-byte AVAILABLE packet
//   * delay 10 ms
//   * receive 2 bytes (big-endian count)
//
// The chip returns 0xFFFF when there's nothing pending. We additionally
// clamp anything > MAX_RECV_SIZE (4095) to 0 because that's bogus and
// usually means we read garbage — better to skip than to ask for 32 KiB
// and corrupt our scratch buffer trying to absorb it.
// ------------------------------------------------------------------
size_t SscmaCamera::bytesAvailable() {
  if (!_ready) return 0;
  if (!syncLineHigh()) return 0;

  // Some boards intermittently return a corrupt AVAILABLE value on the first
  // poll after a mode switch. Retry a few times before giving up.
  for (int attempt = 0; attempt < 3; attempt++) {
    _fillHeader(SSCMA_CMD_AVAILABLE, 0);
    delay(SSCMA_WAIT_MS);
    _spiTx(_txBuf, SSCMA_PACKET_SIZE);
    delay(SSCMA_WAIT_MS);

    uint8_t rx[2] = {0, 0};
    _spiRxOnly(rx, 2);
    uint16_t n = ((uint16_t)rx[0] << 8) | rx[1];
    if (n == 0xFFFF) return 0;
    if (n <= SSCMA_MAX_RECV_SIZE) return n;
    if (_verbose) _log("WARN: bogus AVAILABLE count 0x%04X — retry %d/3", n, attempt + 1);
    delay(2);
  }
  return 0;
}

bool SscmaCamera::readBytes(uint8_t* dst, size_t n) {
  if (!_ready || !dst || n == 0) return false;

  // For each chunk: 10 ms delay, send READ command (256 B), 10 ms
  // delay, receive `want` bytes of data. Matches Seeed driver.
  size_t off = 0;
  while (off < n) {
    size_t want = n - off;
    if (want > SSCMA_MAX_RECV_SIZE) want = SSCMA_MAX_RECV_SIZE;
    _fillHeader(SSCMA_CMD_READ, (uint16_t)want);
    delay(SSCMA_WAIT_MS);
    _spiTx(_txBuf, SSCMA_PACKET_SIZE);
    delay(SSCMA_WAIT_MS);
    _spiRxOnly(dst + off, want);
    off += want;
  }
  return true;
}

bool SscmaCamera::writeBytes(const uint8_t* src, size_t n) {
  if (!_ready || !src || n == 0) return false;

  // For each 250-byte chunk: 10 ms delay, then send the 256-byte WRITE
  // packet. The trailer for a WRITE packet sits AFTER the payload (at
  // [4 + plen][5 + plen]), unlike READ/AVAILABLE where it's at [4][5].
  size_t off = 0;
  while (off < n) {
    size_t plen = n - off;
    if (plen > SSCMA_MAX_PL_LEN) plen = SSCMA_MAX_PL_LEN;
    _fillHeader(SSCMA_CMD_WRITE, (uint16_t)plen);
    memcpy(_txBuf + SSCMA_HEADER_LEN, src + off, plen);
    _txBuf[SSCMA_HEADER_LEN + plen + 0] = 0xFF;
    _txBuf[SSCMA_HEADER_LEN + plen + 1] = 0xFF;
    delay(SSCMA_WAIT_MS);
    _spiTx(_txBuf, SSCMA_PACKET_SIZE);
    off += plen;
  }
  return true;
}

bool SscmaCamera::sendAT(const char* cmd) {
  if (!cmd || !_ready) return false;
  size_t n = strlen(cmd);
  _log("sendAT(%u): %s", (unsigned)n, cmd);
  return writeBytes((const uint8_t*)cmd, n);
}

bool SscmaCamera::setSensor(uint8_t sensorId, bool enable, int optId) {
  if (!_ready) return false;
  flushRx();
  char cmd[48];
  snprintf(cmd, sizeof(cmd), "AT+SENSOR=%u,%d,%d\r\n",
           (unsigned)sensorId, enable ? 1 : 0, optId);
  if (!sendAT(cmd)) return false;
  char resp[384];
  size_t n = readJsonResponse(resp, sizeof(resp), 3000, false);
  if (n == 0) {
    _log("setSensor: no JSON reply");
    return false;
  }
  if (strstr(resp, "\"code\": 0") != nullptr ||
      strstr(resp, "\"code\":0") != nullptr) {
    delay(120);   // imager pipeline settle (factory uses multi-step timers)
    return true;
  }
  _log("setSensor: unexpected reply (first 80 chars)");
  if (_verbose) {
    size_t show = n < 80 ? n : 80;
    Serial.write((const uint8_t*)resp, show);
    Serial.println();
  }
  return false;
}

void SscmaCamera::flushRx() {
  if (!_ready) return;
  _rxScratch = "";
  // Drain whatever the chip already has buffered.
  uint32_t guard = millis() + 200;
  while ((int32_t)(millis() - guard) < 0) {
    size_t avail = bytesAvailable();
    if (!avail) break;
    if (avail > SSCMA_MAX_RECV_SIZE) avail = SSCMA_MAX_RECV_SIZE;
    static uint8_t scratch[SSCMA_MAX_RECV_SIZE];
    readBytes(scratch, avail);
  }
}

size_t SscmaCamera::readJsonResponse(char* outBuf, size_t maxLen,
                                     uint32_t timeoutMs, bool debug) {
  if (!outBuf || maxLen == 0 || !_ready) return 0;
  uint32_t start = millis();
  static uint8_t chunk[SSCMA_MAX_RECV_SIZE];

  // FIRST: see if there's already a complete frame in leftover scratch from a
  // previous call. This is the common case for AT+SAMPLE which produces TWO
  // envelopes back-to-back: the ACK and the image. After we consume the ACK,
  // the image envelope is already sitting in scratch, ready to parse.
  auto tryParse = [&](void) -> int {
    const char* base = _rxScratch.c_str();
    size_t scratchLen = _rxScratch.length();
    const char* suffix = strstr(base, SSCMA_FRAME_SUFFIX);
    if (!suffix) return -1;
    const char* prefix = nullptr;
    for (const char* p = base; p < suffix; p++) {
      if (p[0] == '\r' && p[1] == '{') prefix = p;
    }
    if (!prefix) return -1;
    size_t jsonStart = (size_t)(prefix - base) + 1;
    size_t jsonEnd   = (size_t)(suffix - base) + 1;
    size_t n = jsonEnd - jsonStart;
    if (n >= maxLen) n = maxLen - 1;
    memcpy(outBuf, base + jsonStart, n);
    outBuf[n] = '\0';
    size_t consumed = (size_t)(suffix - base) + 2;
    if (consumed > scratchLen) consumed = scratchLen;
    _rxScratch.remove(0, consumed);
    return (int)n;
  };

  int already = tryParse();
  if (already >= 0) return (size_t)already;

  uint32_t lastProgressAt = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    size_t avail = bytesAvailable();
    if (avail == 0) {
      // Recovery path: if we have been stalled for a while but SYNC is still
      // asserted, force a bounded READ. This helps when AVAILABLE gets noisy.
      if ((uint32_t)(millis() - lastProgressAt) > 180 && syncLineHigh()) {
        size_t forced = 256;
        if (forced > sizeof(chunk)) forced = sizeof(chunk);
        if (readBytes(chunk, forced)) {
          if (_rxScratch.length() + forced > SCRATCH_HARD_CAP) {
            _log("ERR: scratch cap hit during forced-read");
            return 0;
          }
          size_t keptForced = 0;
          for (size_t i = 0; i < forced; i++) {
            uint8_t b = chunk[i];
            if (b == 0x00) continue;
            _rxScratch.concat((char)b);
            keptForced++;
          }
          if (debug && keptForced > 0) {
            Serial.printf("[sscma]   forced-read kept=%u\n", (unsigned)keptForced);
          }
          int got = tryParse();
          if (got >= 0) return (size_t)got;
          if (keptForced > 0) lastProgressAt = millis();
        }
      }
      delay(SSCMA_WAIT_MS);
      continue;
    }
    size_t want = avail;
    if (want > sizeof(chunk)) want = sizeof(chunk);
    if (!readBytes(chunk, want)) break;

    // Append to scratch — STRIPPING 0x00 padding. The Himax often pads
    // short reads with 0x00, and Arduino String stores them faithfully but
    // `indexOf()` uses strstr() under the hood which BAILS at the first
    // NUL byte. The official Seeed driver (sscma_client_ops.c, line 237)
    // does the exact same NUL-strip before scanning for frames.
    size_t L = _rxScratch.length();
    if (L + want > SCRATCH_HARD_CAP) {
      _log("ERR: scratch buffer hit %u-byte cap without framing",
           (unsigned)SCRATCH_HARD_CAP);
      return 0;
    }
    size_t kept = 0;
    for (size_t i = 0; i < want; i++) {
      uint8_t b = chunk[i];
      if (b == 0x00) continue;          // SPI padding
      _rxScratch.concat((char)b);
      kept++;
    }

    if (debug) {
      size_t showFrom = _rxScratch.length() > 64 ? _rxScratch.length() - 64 : 0;
      Serial.printf("[sscma]   rx +%u (kept %u, len=%u) tail:",
                    (unsigned)want, (unsigned)kept,
                    (unsigned)_rxScratch.length());
      for (size_t i = showFrom; i < _rxScratch.length(); i++) {
        Serial.printf(" %02X", (uint8_t)_rxScratch.charAt(i));
      }
      Serial.println();
    }

    int got = tryParse();
    if (got >= 0) return (size_t)got;
    if (kept > 0) lastProgressAt = millis();
  }
  if (_verbose) {
    _log("readJsonResponse TIMEOUT after %u ms (scratch=%u bytes)",
         (unsigned)timeoutMs, (unsigned)_rxScratch.length());
    if (_rxScratch.length()) {
      size_t show = _rxScratch.length() < 64 ? _rxScratch.length() : 64;
      Serial.print(F("[sscma]   tail (hex):"));
      for (size_t i = _rxScratch.length() - show; i < _rxScratch.length(); i++) {
        Serial.printf(" %02X", (uint8_t)_rxScratch.charAt(i));
      }
      Serial.println();
    }
  }
  return 0;
}

// ------------------------------------------------------------------
// Base64 (RFC 4648)
// ------------------------------------------------------------------
static int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static size_t b64Decode(const char* in, size_t inLen, uint8_t* out, size_t outCap) {
  size_t oi = 0;
  int vals[4];
  int vi = 0;
  for (size_t i = 0; i < inLen; i++) {
    char c = in[i];
    if (c == '=' || c == '\0') break;
    if (c == '\r' || c == '\n' || c == ' ') continue;
    int v = b64Val(c);
    if (v < 0) continue;
    vals[vi++] = v;
    if (vi == 4) {
      if (oi + 3 > outCap) return oi;
      out[oi++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
      out[oi++] = (uint8_t)(((vals[1] & 0xF) << 4) | (vals[2] >> 2));
      out[oi++] = (uint8_t)(((vals[2] & 0x3) << 6) | vals[3]);
      vi = 0;
    }
  }
  if (vi == 2 && oi + 1 <= outCap) {
    out[oi++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
  } else if (vi == 3 && oi + 2 <= outCap) {
    out[oi++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
    out[oi++] = (uint8_t)(((vals[1] & 0xF) << 4) | (vals[2] >> 2));
  }
  return oi;
}

bool SscmaCamera::captureJpeg(uint8_t* jpegOut, size_t jpegMaxLen,
                              size_t* jpegLen, uint32_t timeoutMs, bool debug) {
  if (!jpegOut || !jpegLen || jpegMaxLen == 0 || !_ready) return false;
  *jpegLen = 0;

  // One SSCMA JSON envelope can embed a huge base64 JPEG (640x480 path).
  const size_t JSON_CAP = 240 * 1024;
  char* json = (char*)ps_malloc(JSON_CAP);
  if (!json) json = (char*)malloc(JSON_CAP);
  if (!json) {
    _log("captureJpeg: out of memory for json buf (%u bytes)",
         (unsigned)JSON_CAP);
    return false;
  }

  flushRx();
  // See Seeed tf_module_ai_camera.c: AT+SENSOR=1,1,<opt> before SAMPLE.
  if (!setSensor(1, true, SSCMA_CAPTURE_SENSOR_OPT)) {
    _log("WARN: AT+SENSOR failed — continuing with AT+SAMPLE anyway");
  }

  if (!sendAT("AT+SAMPLE=1\r\n")) {
    _log("AT+SAMPLE write failed");
    free(json);
    return false;
  }

  uint32_t start = millis();
  bool ok = false;
  int envelopes = 0;
  while ((uint32_t)(millis() - start) < timeoutMs) {
    uint32_t remain = timeoutMs - (uint32_t)(millis() - start);
    if (remain < 500) break;
    size_t got = readJsonResponse(json, JSON_CAP, remain, debug);
    if (got == 0) continue;
    envelopes++;
    if (debug) {
      _log("envelope #%d: %u bytes", envelopes, (unsigned)got);
      Serial.print(F("[sscma]   first 120 chars: "));
      Serial.write((const uint8_t*)json, got < 120 ? got : 120);
      Serial.println();
    }
    const char* keyPos = strstr(json, "\"image\"");
    if (!keyPos) continue;
    const char* cPos = strstr(json, "\"count\"");
    if (cPos) {
      const char* cc = strchr(cPos, ':');
      if (cc) {
        int c = atoi(cc + 1);
        _log("frame count=%d", c);
      }
    }
    const char* colon = strchr(keyPos, ':');
    if (!colon) continue;
    const char* q1 = strchr(colon, '\"');
    if (!q1) continue;
    const char* q2 = strchr(q1 + 1, '\"');
    if (!q2) continue;
    size_t b64Len = (size_t)(q2 - q1 - 1);
    _log("found image field: %u base64 chars", (unsigned)b64Len);
    size_t decoded = b64Decode(q1 + 1, b64Len, jpegOut, jpegMaxLen);
    _log("base64 -> %u JPEG bytes", (unsigned)decoded);
    if (decoded < 4) {
      _log("ERR: decode produced too few bytes");
      break;
    }
    if (jpegOut[0] != 0xFF || jpegOut[1] != 0xD8 || jpegOut[2] != 0xFF) {
      _log("ERR: not a JPEG (first bytes %02X %02X %02X)",
           jpegOut[0], jpegOut[1], jpegOut[2]);
      break;
    }
    *jpegLen = decoded;
    ok = true;
    break;
  }
  // One fast retry: some firmware builds occasionally ACK SAMPLE but delay
  // the image event long enough to miss the first pass.
  if (!ok) {
    _log("captureJpeg first pass failed, retrying AT+SAMPLE once");
    flushRx();
    if (sendAT("AT+SAMPLE=1\r\n")) {
      uint32_t retryStart = millis();
      while ((uint32_t)(millis() - retryStart) < 4000) {
        size_t got = readJsonResponse(json, JSON_CAP, 4000, debug);
        if (got == 0) continue;
        const char* keyPos = strstr(json, "\"image\"");
        if (!keyPos) continue;
        const char* colon = strchr(keyPos, ':');
        if (!colon) continue;
        const char* q1 = strchr(colon, '\"');
        if (!q1) continue;
        const char* q2 = strchr(q1 + 1, '\"');
        if (!q2) continue;
        size_t b64Len = (size_t)(q2 - q1 - 1);
        size_t decoded = b64Decode(q1 + 1, b64Len, jpegOut, jpegMaxLen);
        if (decoded >= 4 &&
            jpegOut[0] == 0xFF && jpegOut[1] == 0xD8 && jpegOut[2] == 0xFF) {
          *jpegLen = decoded;
          ok = true;
          break;
        }
      }
    }
  }
  free(json);
  if (!ok) _log("captureJpeg TIMEOUT after %u ms (saw %d envelope%s)",
                (unsigned)timeoutMs, envelopes, envelopes == 1 ? "" : "s");
  return ok;
}

bool SscmaCamera::captureJpegInvoke(uint8_t* jpegOut, size_t jpegMaxLen,
                                    size_t* jpegLen, int times, bool filter,
                                    bool show, uint32_t timeoutMs, bool debug) {
  if (!jpegOut || !jpegLen || jpegMaxLen == 0 || !_ready) return false;
  *jpegLen = 0;

  const size_t JSON_CAP = 240 * 1024;
  char* json = (char*)ps_malloc(JSON_CAP);
  if (!json) json = (char*)malloc(JSON_CAP);
  if (!json) {
    _log("captureJpegInvoke: OOM json buf (%u bytes)", (unsigned)JSON_CAP);
    return false;
  }

  flushRx();
  // Inference path uses 416x416 in factory firmware.
  if (!setSensor(1, true, SSCMA_SENSOR_OPT_416_416)) {
    _log("WARN: AT+SENSOR 416 failed — trying INVOKE anyway");
  }

  char cmd[48];
  snprintf(cmd, sizeof(cmd), "AT+INVOKE=%d,%d,%d\r\n", times, filter ? 1 : 0,
           show ? 0 : 1);
  if (!sendAT(cmd)) {
    _log("AT+INVOKE write failed");
    free(json);
    return false;
  }

  uint32_t start = millis();
  bool ok = false;
  int envelopes = 0;
  while ((uint32_t)(millis() - start) < timeoutMs) {
    uint32_t remain = timeoutMs - (uint32_t)(millis() - start);
    if (remain < 500) break;
    size_t got = readJsonResponse(json, JSON_CAP, remain, debug);
    if (got == 0) continue;
    envelopes++;
    if (debug) {
      _log("invoke env #%d: %u bytes", envelopes, (unsigned)got);
    }
    const char* keyPos = strstr(json, "\"image\"");
    if (!keyPos) continue;
    const char* cPos = strstr(json, "\"count\"");
    if (cPos) {
      const char* cc = strchr(cPos, ':');
      if (cc) {
        int c = atoi(cc + 1);
        _log("invoke frame count=%d", c);
      }
    }
    const char* colon = strchr(keyPos, ':');
    if (!colon) continue;
    const char* q1 = strchr(colon, '\"');
    if (!q1) continue;
    const char* q2 = strchr(q1 + 1, '\"');
    if (!q2) continue;
    size_t b64Len = (size_t)(q2 - q1 - 1);
    size_t decoded = b64Decode(q1 + 1, b64Len, jpegOut, jpegMaxLen);
    if (decoded < 4) continue;
    if (jpegOut[0] != 0xFF || jpegOut[1] != 0xD8 || jpegOut[2] != 0xFF)
      continue;
    *jpegLen = decoded;
    ok = true;
    break;
  }
  free(json);
  if (!ok)
    _log("captureJpegInvoke TIMEOUT after %u ms (envelopes=%d)",
         (unsigned)timeoutMs, envelopes);
  return ok;
}

void SscmaCamera::dumpStatus() {
  Serial.println(F("[sscma] --- status ---"));
  Serial.printf("[sscma] ready=%s alive=%s mode=%d\n",
                _ready ? "YES" : "no", _alive ? "YES" : "no", (int)_spiMode);
  uint16_t in = 0, out = 0, cfg = 0;
  pca9535_read16(PCA9535_IN_PORT,  &in);
  pca9535_read16(PCA9535_OUT_PORT, &out);
  pca9535_read16(PCA9535_CFG_PORT, &cfg);
  Serial.printf("[sscma] PCA9535  IN=0x%04X  OUT=0x%04X  CFG=0x%04X\n",
                in, out, cfg);
  Serial.printf("[sscma]   AI_CHIP pwr (bit 11) = %s\n",
                (out & BSP_PWR_AI_CHIP) ? "ON" : "OFF");
  Serial.printf("[sscma]   RST  (bit %d)        = %s   (HIGH == running)\n",
                SSCMA_RST_EXP_PIN,
                (out & (1u << SSCMA_RST_EXP_PIN)) ? "HIGH" : "LOW");
  Serial.printf("[sscma]   SYNC (bit %d, in)    = %s   (HIGH == data ready)\n",
                SSCMA_SYNC_EXP_PIN,
                (in & (1u << SSCMA_SYNC_EXP_PIN)) ? "HIGH" : "low");
  Serial.printf("[sscma] SPI: SCLK=%d MOSI=%d MISO=%d CS=%d clk=%dHz\n",
                SSCMA_SPI_SCLK_PIN, SSCMA_SPI_MOSI_PIN, SSCMA_SPI_MISO_PIN,
                SSCMA_SPI_CS_PIN, SSCMA_SPI_CLOCK_HZ);
  Serial.printf("[sscma] scratch=%u bytes\n", (unsigned)_rxScratch.length());
  // One-shot AVAILABLE poll
  size_t a = bytesAvailable();
  Serial.printf("[sscma] one-shot AVAILABLE = %u bytes\n", (unsigned)a);
}

void SscmaCamera::blindAt(const char* at) {
  if (!_ready) {
    Serial.println(F("[sscma] blindAt: not ready (run caminit first)"));
    return;
  }
  if (!at) at = "AT+ID?\r\n";
  Serial.printf("[sscma] blindAt: sending '%s'\n", at);
  flushRx();
  size_t n = strlen(at);
  if (!writeBytes((const uint8_t*)at, n)) {
    Serial.println(F("[sscma]   write failed"));
    return;
  }
  // Poll for up to 1.5s, dump everything we get.
  static uint8_t buf[SSCMA_MAX_RECV_SIZE];
  uint32_t end = millis() + 1500;
  uint32_t total = 0;
  while ((int32_t)(millis() - end) < 0) {
    size_t avail = bytesAvailable();
    if (avail == 0) { delay(SSCMA_WAIT_MS); continue; }
    if (avail > sizeof(buf)) avail = sizeof(buf);
    if (!readBytes(buf, avail)) break;
    total += avail;
    Serial.printf("[sscma]   chunk %u bytes, hex:", (unsigned)avail);
    size_t show = avail < 96 ? avail : 96;
    for (size_t i = 0; i < show; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
    Serial.print(F("[sscma]   asc: '"));
    for (size_t i = 0; i < show; i++) {
      char c = (char)buf[i];
      Serial.print(isprint((uint8_t)c) ? c : '.');
    }
    Serial.println('\'');
  }
  Serial.printf("[sscma] blindAt total %u bytes\n", (unsigned)total);
}

void SscmaCamera::coldBootProbe(uint32_t observeMs) {
  if (!_ready) {
    Serial.println(F("[sscma] coldBootProbe: not ready (run caminit first)"));
    return;
  }
  Serial.printf("[sscma] coldBootProbe: pulse RST, watch SPI for %u ms\n",
                (unsigned)observeMs);
  hardReset();
  delay(800);                           // banner phase finishes here
  flushRx();
  static uint8_t buf[SSCMA_MAX_RECV_SIZE];
  uint32_t end = millis() + observeMs;
  uint32_t total = 0;
  while ((int32_t)(millis() - end) < 0) {
    size_t avail = bytesAvailable();
    if (avail == 0) { delay(SSCMA_WAIT_MS); continue; }
    if (avail > sizeof(buf)) avail = sizeof(buf);
    if (!readBytes(buf, avail)) break;
    total += avail;
    Serial.printf("[sscma]   chunk %u (total %u):", (unsigned)avail, (unsigned)total);
    size_t show = avail < 64 ? avail : 64;
    for (size_t i = 0; i < show; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
  }
  Serial.printf("[sscma] coldBootProbe total %u bytes\n", (unsigned)total);
  if (total == 0) {
    Serial.println(F("[sscma]   silence — chip might not be running SSCMA on SPI."));
    Serial.println(F("[sscma]   try `cambus` and `camsweep`."));
  }
}

void SscmaCamera::busProbe() {
  if (!_ready) {
    Serial.println(F("[sscma] busProbe: not ready (run caminit first)"));
    return;
  }
  Serial.println(F("[sscma] busProbe: low-level wiggle test"));
  bool h;
  if (_expGetLevel(SSCMA_SYNC_EXP_PIN, h)) {
    Serial.printf("[sscma]   SYNC line: %s\n", h ? "HIGH (data ready)" : "low");
  } else {
    Serial.println(F("[sscma]   SYNC read failed"));
  }
  Serial.printf("[sscma]   CS pin %d level read: %d\n",
                SSCMA_SPI_CS_PIN, digitalRead(SSCMA_SPI_CS_PIN));
  // Try a single AVAILABLE
  size_t a = bytesAvailable();
  Serial.printf("[sscma]   AVAILABLE = %u\n", (unsigned)a);
  // Pump the bus a bit
  uint8_t txAllZero[16] = {0};
  uint8_t rx[16] = {0};
  _spiTxRx(txAllZero, rx, 16);
  Serial.print(F("[sscma]   16-byte loopback (zeros sent), miso ="));
  for (int i = 0; i < 16; i++) Serial.printf(" %02X", rx[i]);
  Serial.println();
}

void SscmaCamera::modeSweep() {
  if (!_ready) {
    Serial.println(F("[sscma] modeSweep: not ready (run caminit first)"));
    return;
  }
  Serial.println(F("[sscma] modeSweep: trying SPI modes 0..3"));
  uint8_t saved = _spiMode;
  for (int m = 0; m < 4; m++) {
    _spiMode = (m == 0) ? SPI_MODE0 : (m == 1) ? SPI_MODE1 :
               (m == 2) ? SPI_MODE2 :  SPI_MODE3;
    Serial.printf("[sscma]   --- mode %d ---\n", m);
    hardReset();
    delay(800);
    flushRx();
    if (!sendAT("AT+ID?\r\n")) continue;
    char resp[256];
    size_t got = readJsonResponse(resp, sizeof(resp), 2000, false);
    if (got > 0) {
      Serial.printf("[sscma]   mode %d -> RESPONSE: %s\n", m, resp);
      Serial.printf("[sscma]   *** WORKING MODE FOUND: %d ***\n", m);
      saved = _spiMode;       // adopt this mode
      break;
    } else {
      Serial.printf("[sscma]   mode %d -> no response\n", m);
    }
  }
  _spiMode = saved;
}
