// lcd_flush.h - fast, optionally asynchronous transfer of the Arduino_Canvas
// framebuffer to the AXS15231B panel over QSPI.
//
// Why: the library's Arduino_Canvas::flush() byte-swaps 1024 pixels at a time
// into a DMA buffer and then waits for each transfer, so CPU copy and bus
// time add up to ~46 ms per frame.
//
// flush():   synchronous, pipelined. Two DMA buffers ping-pong, the next
//            chunk is byte-swapped while the previous one is on the bus.
// present(): asynchronous, double buffered. The finished frame is handed to a
//            flush task on core 0 and drawing continues immediately in a
//            second PSRAM framebuffer (the canvas and gfx are re-pointed to
//            it). Only blocks if the previous frame is still being sent.
//            => every frame must repaint the WHOLE screen (the back buffer
//            holds the frame before last).
//
// Both talk to the library's own SPI device (same bus settings and command
// protocol as Arduino_ESP32QSPI::writePixels). After the first present()
// the library's display.flush() must not be used any more (it would race
// with the flush task). Call from the UI task only.
#pragma once

#include <Arduino.h>

class JC3248W535EN;

namespace lcd {

// Grabs the bus handle, allocates 2 x 8 KB DMA buffers and, if `async`, a
// second 300 KB PSRAM framebuffer + the flush task. Call after
// display.begin(). Returns false if the fast path is unavailable (then
// flush()/present() fall back to display.flush()).
bool begin(JC3248W535EN &display, bool async = true);

// Synchronous full-frame transfer.
void flush();

// Asynchronous: send the current frame, switch drawing to the other buffer.
// Falls back to flush() when async is not available.
void present();

// Block until the flush task is idle (e.g. before sleeping the panel).
void waitIdle();

// Framebuffer currently being drawn (same as gfx::framebuffer()).
uint16_t *backBuffer();

bool fast();          // fast path active
bool async();         // double buffering active
void setFast(bool on);  // false = library flush (for comparisons)

// Timing of the last fast transfer (microseconds) and the SPI clock.
struct Stats {
  uint32_t total;     // whole transfer
  uint32_t copy;      // CPU byte-swap time
  uint32_t wait;      // waiting for the bus
  uint32_t presentWait;  // last present(): time blocked on the previous frame
  int spiKHz;
};
Stats stats();

}  // namespace lcd
