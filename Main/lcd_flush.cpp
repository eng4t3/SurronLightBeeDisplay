// lcd_flush.cpp - see lcd_flush.h.
#include "lcd_flush.h"

#include <JC3248W535EN_Touch_LCD.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "gfx.h"

namespace {

// ---------------------------------------------------------------------------
// Access to private/protected members of the display libraries (bus object,
// SPI device handle, CS pin, canvas framebuffer pointer). Explicit template
// instantiation may name inaccessible members, so this reads them without
// patching the libraries.
// ---------------------------------------------------------------------------
template <typename Tag, typename Tag::type M>
struct Grab {
  friend typename Tag::type member(Tag) { return M; }
};
struct BusTag {
  typedef Arduino_ESP32QSPI *JC3248W535EN::*type;
  friend type member(BusTag);
};
struct HandleTag {
  typedef spi_device_handle_t Arduino_ESP32QSPI::*type;
  friend type member(HandleTag);
};
struct CsTag {
  typedef int8_t Arduino_ESP32QSPI::*type;
  friend type member(CsTag);
};
struct FbTag {
  typedef uint16_t *Arduino_Canvas::*type;
  friend type member(FbTag);
};
template struct Grab<BusTag, &JC3248W535EN::bus>;
template struct Grab<HandleTag, &Arduino_ESP32QSPI::_handle>;
template struct Grab<CsTag, &Arduino_ESP32QSPI::_cs>;
template struct Grab<FbTag, &Arduino_Canvas::_framebuffer>;

constexpr int NATIVE_W = 320, NATIVE_H = 480;
constexpr uint32_t PIXELS = (uint32_t)NATIVE_W * NATIVE_H;
constexpr uint32_t CHUNK = 4096;  // pixels per DMA transfer (bus max is 8196)

JC3248W535EN *s_disp = nullptr;
spi_device_handle_t s_dev = nullptr;
int s_cs = -1;
bool s_enabled = true;
uint32_t *s_buf[2] = {nullptr, nullptr};
spi_transaction_ext_t s_t[2];
spi_transaction_ext_t s_cmd;
lcd::Stats s_stats = {};

// async state
bool s_async = false;
uint16_t *s_fbs[2] = {nullptr, nullptr};
int s_back = 0;
const uint16_t *volatile s_front = nullptr;
SemaphoreHandle_t s_go = nullptr, s_done = nullptr;

// Single-line command with 4 data bytes (CASET / RASET), as writeC8D16D16().
void cmdC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) {
  memset(&s_cmd, 0, sizeof(s_cmd));
  s_cmd.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  s_cmd.base.cmd = 0x02;
  s_cmd.base.addr = ((uint32_t)c) << 8;
  s_cmd.base.tx_data[0] = d1 >> 8;
  s_cmd.base.tx_data[1] = d1;
  s_cmd.base.tx_data[2] = d2 >> 8;
  s_cmd.base.tx_data[3] = d2;
  s_cmd.base.length = 32;
  digitalWrite(s_cs, LOW);
  spi_device_polling_transmit(s_dev, &s_cmd.base);
  digitalWrite(s_cs, HIGH);
}

// Single-line command without data, as Arduino_ESP32QSPI::writeCommand().
void cmdC8(uint8_t c) {
  memset(&s_cmd, 0, sizeof(s_cmd));
  s_cmd.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  s_cmd.base.cmd = 0x02;
  s_cmd.base.addr = ((uint32_t)c) << 8;
  s_cmd.base.length = 0;
  digitalWrite(s_cs, LOW);
  spi_device_polling_transmit(s_dev, &s_cmd.base);
  digitalWrite(s_cs, HIGH);
}

// RGB565 little endian (framebuffer) -> big endian (panel), 2 px per word.
inline void swapCopy(uint32_t *dst, const uint32_t *src, uint32_t words) {
  while (words >= 4) {
    const uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
    dst[0] = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
    dst[1] = ((b & 0x00FF00FFu) << 8) | ((b >> 8) & 0x00FF00FFu);
    dst[2] = ((c & 0x00FF00FFu) << 8) | ((c >> 8) & 0x00FF00FFu);
    dst[3] = ((d & 0x00FF00FFu) << 8) | ((d >> 8) & 0x00FF00FFu);
    src += 4;
    dst += 4;
    words -= 4;
  }
  while (words--) {
    const uint32_t a = *src++;
    *dst++ = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
  }
}

// Sends one native 320x480 frame. Interrupt-driven transfers (the CPU sleeps
// instead of spinning while a chunk is on the bus); the device queue holds
// one transaction, which is exactly the ping-pong we need.
void sendFrame(const uint16_t *frame) {
  const uint32_t *fb = (const uint32_t *)frame;
  uint32_t tCopy = 0, tWait = 0;
  const uint32_t t0 = micros();
  // Same sequence as Arduino_AXS15231B::writeAddrWindow(): CASET, RASET and
  // RAMWR (0x2C) to reset the controller's write pointer to the window start.
  // The pixel stream below uses 0x3C (memory write CONTINUE); without the
  // RAMWR every frame continued from wherever the pointer was, which showed
  // up on the panel as shifted / tiled copies of the frame.
  cmdC8D16D16(0x2A, 0, NATIVE_W - 1);  // CASET (full screen)
  cmdC8D16D16(0x2B, 0, NATIVE_H - 1);  // RASET
  cmdC8(0x2C);                         // RAMWR
  uint32_t done = 0;
  int cur = 0;
  bool inFlight = false;
  digitalWrite(s_cs, LOW);
  while (done < PIXELS) {
    const uint32_t n = (PIXELS - done) > CHUNK ? CHUNK : (PIXELS - done);
    uint32_t ta = micros();
    swapCopy(s_buf[cur], fb + done / 2, n / 2);  // overlaps the previous transfer
    uint32_t tb = micros();
    tCopy += tb - ta;
    spi_transaction_t *rt;
    if (inFlight) spi_device_get_trans_result(s_dev, &rt, portMAX_DELAY);
    tWait += micros() - tb;
    spi_transaction_ext_t &t = s_t[cur];
    memset(&t, 0, sizeof(t));
    if (done == 0) {
      t.base.flags = SPI_TRANS_MODE_QIO;
      t.base.cmd = 0x32;
      t.base.addr = 0x003C00;  // 0x3C = memory write continue (after RAMWR above)
    } else {
      t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                     SPI_TRANS_VARIABLE_DUMMY;
    }
    t.base.tx_buffer = s_buf[cur];
    t.base.length = n * 16;
    spi_device_queue_trans(s_dev, &t.base, portMAX_DELAY);
    inFlight = true;
    done += n;
    cur ^= 1;
  }
  const uint32_t te = micros();
  spi_transaction_t *rt;
  if (inFlight) spi_device_get_trans_result(s_dev, &rt, portMAX_DELAY);
  digitalWrite(s_cs, HIGH);
  tWait += micros() - te;
  s_stats.total = micros() - t0;
  s_stats.copy = tCopy;
  s_stats.wait = tWait;
}

void flushTask(void *) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    sendFrame(s_front);
    xSemaphoreGive(s_done);
  }
}

void pointTo(uint16_t *fb) {
  s_disp->gfx->*member(FbTag()) = fb;
  gfx::setFramebuffer(fb);
}

}  // namespace

namespace lcd {

bool begin(JC3248W535EN &display, bool async) {
  s_disp = &display;
  Arduino_ESP32QSPI *bus = display.*member(BusTag());
  if (!bus || !display.gfx || !display.gfx->getFramebuffer()) return false;
  s_dev = bus->*member(HandleTag());
  s_cs = bus->*member(CsTag());
  if (!s_dev || s_cs < 0) {
    s_dev = nullptr;
    return false;
  }
  for (int i = 0; i < 2; i++) {
    if (!s_buf[i]) s_buf[i] = (uint32_t *)heap_caps_aligned_alloc(16, CHUNK * 2, MALLOC_CAP_DMA);
    if (!s_buf[i]) {
      s_dev = nullptr;
      return false;
    }
  }
  spi_device_get_actual_freq(s_dev, &s_stats.spiKHz);
  if (async && !s_go) {
    s_fbs[0] = display.gfx->getFramebuffer();
    s_fbs[1] = (uint16_t *)heap_caps_aligned_alloc(16, PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (s_fbs[1] && s_go && s_done) {
      xSemaphoreGive(s_done);  // idle
      // Core 0 (the UI runs on core 1). Priority above the Arduino loop /
      // web server, below the WiFi/lwIP tasks.
      if (xTaskCreatePinnedToCore(flushTask, "lcdFlush", 3072, nullptr, 3, nullptr, 0) == pdPASS) {
        s_async = true;
        s_back = 0;
      }
    }
  }
  return true;
}

bool fast() { return s_dev != nullptr && s_enabled; }
bool async() { return s_async && fast(); }
void setFast(bool on) {
  if (!on) waitIdle();
  s_enabled = on;
}
Stats stats() { return s_stats; }
uint16_t *backBuffer() { return s_disp && s_disp->gfx ? s_disp->gfx->getFramebuffer() : nullptr; }

void waitIdle() {
  if (s_async && s_done) {
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_done);
  }
}

void flush() {
  if (!s_disp) return;
  if (!fast()) {
    waitIdle();
    s_disp->flush();
    return;
  }
  waitIdle();
  sendFrame(s_disp->gfx->getFramebuffer());
}

void present() {
  if (!s_disp) return;
  if (!async()) {
    flush();
    return;
  }
  const uint32_t t0 = micros();
  xSemaphoreTake(s_done, portMAX_DELAY);  // previous frame fully sent
  s_stats.presentWait = micros() - t0;
  s_front = s_fbs[s_back];
  xSemaphoreGive(s_go);
  s_back ^= 1;
  pointTo(s_fbs[s_back]);
}

}  // namespace lcd
