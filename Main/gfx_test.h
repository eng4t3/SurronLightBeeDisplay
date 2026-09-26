// gfx_test.h - TEMPORARY engine test cards and micro-benchmarks, driven by
// the debug console (`gfxtest`, `bench`). Safe to delete together with the
// two calls in debug_console.cpp once the new UI is in place.
#pragma once

#include <Arduino.h>

namespace gfxtest {
constexpr int PAGES = 4;
// Renders test card `page` (0..PAGES-1) into the gfx framebuffer.
void draw(int page);
// Runs timing benchmarks (draws garbage into the framebuffer) and prints them.
void bench(Print &out);
}  // namespace gfxtest
