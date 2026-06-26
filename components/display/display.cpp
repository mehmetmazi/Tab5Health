/*
 * Display foundation: M5GFX (ST7121 panel + touch) + LVGL 9.
 * Owns the LVGL rendering task and the lock that guards all lv_* access.
 */
#include "display.hpp"
#include <M5GFX.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "display";
static M5GFX s_gfx;
static SemaphoreHandle_t s_lock;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    s_gfx.startWrite();
    s_gfx.pushImage(area->x1, area->y1, w, h, reinterpret_cast<const lgfx::rgb565_t *>(px_map));
    s_gfx.endWrite();
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void touch_cb(lv_indev_t *, lv_indev_data_t *data)
{
    int32_t x = 0, y = 0;
    if (s_gfx.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Screensaver: blank the backlight after idle, wake on touch. The touch panel
// keeps polling while the backlight is off, so any tap resets LVGL's inactivity
// timer and the next tick turns the backlight back on.
static bool           s_screen_on   = true;
static const uint32_t SCREENSAVER_MS = 60000;   // 60 s of no touch -> sleep

static void screensaver_cb(lv_timer_t *)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (idle > SCREENSAVER_MS && s_screen_on) {
        s_gfx.setBrightness(0);
        s_screen_on = false;
    } else if (idle <= SCREENSAVER_MS && !s_screen_on) {
        s_gfx.setBrightness(255);
        s_screen_on = true;
    }
}

bool display_lock(uint32_t timeout_ms)
{
    const TickType_t t = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE;
}

void display_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static void lvgl_task(void *)
{
    while (true) {
        uint32_t idle = 50;
        if (display_lock(0)) {
            idle = lv_timer_handler();
            display_unlock();
        }
        if (idle > 50) idle = 50;
        if (idle < 5)  idle = 5;
        vTaskDelay(pdMS_TO_TICKS(idle));
    }
}

esp_err_t display_init(void)
{
    s_gfx.init();
    s_gfx.setRotation(1);                              /* landscape 1280x720 */
    s_gfx.fillScreen(TFT_BLACK);                       /* clear power-on framebuffer garbage */
    const int32_t W = s_gfx.width();
    const int32_t H = s_gfx.height();
    ESP_LOGI(TAG, "M5GFX %dx%d", (int)W, (int)H);

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* Full-screen buffers + FULL render mode: render and flush the whole frame
       at once. Avoids partial-band rendering (which was leaving a bottom strip)
       and there's ample PSRAM (2 x 1280x720x2 ~= 3.7 MB of 32 MB). */
    const size_t buf_bytes = (size_t)W * H * 2;        /* full-screen RGB565 */
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!b1 || !b2) { ESP_LOGE(TAG, "draw-buffer alloc failed"); return ESP_ERR_NO_MEM; }

    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    lv_timer_create(screensaver_cb, 500, nullptr);   // backlight idle-off / touch-wake

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 4, nullptr, tskNO_AFFINITY);

    ESP_LOGI(TAG, "LVGL up");
    return ESP_OK;
}
