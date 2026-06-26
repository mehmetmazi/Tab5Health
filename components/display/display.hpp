#pragma once
#include "esp_err.h"
#include <cstdint>

// Brings up M5GFX (ST7121 panel + touch, landscape 1280x720) and LVGL, then
// starts the LVGL rendering task. Call once at startup before any lv_* calls.
esp_err_t display_init();

// LVGL is not thread-safe and runs on its own task. Any code touching lv_*
// objects from another task MUST hold this (recursive) lock.
// timeout_ms == 0 blocks until acquired.
bool display_lock(uint32_t timeout_ms);
void display_unlock();
