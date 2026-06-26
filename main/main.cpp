/*
 * M5Stack Tab5 (ESP32-P4) — Health dashboard.
 *
 * Brings up the display + INA226 battery monitor, builds the dashboard, then on a
 * 10-minute timer connects Wi-Fi (via the C6), fetches Oura + Withings, reads the
 * battery, and refreshes the tiles.
 */
#include "display.hpp"
#include "ui.hpp"
#include "model.hpp"
#include "battery.hpp"
#include "net.hpp"
#include "wifi_credentials.h"
#include "oura.hpp"
#include "oura_credentials.h"
#include "withings.hpp"
#include "withings_credentials.h"
#include "weather.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>
#include <cstdlib>

static const char *TAG = "app";

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Tab5 health dashboard — Milestone B (mock data)");

    ESP_ERROR_CHECK(display_init());
    battery_init();

    HealthSnapshot snap{};
    snap.sleep_score  = { 82.0f,   true, 0 };
    snap.hrv_ms       = { 45.0f,   true, 0 };
    snap.resting_hr   = { 52.0f,   true, 0 };
    snap.readiness    = { 88.0f,   true, 0 };
    snap.weight_kg    = { 74.2f,   true, 0 };
    snap.body_fat_pct = { 18.5f,   true, 0 };
    snap.steps        = { 8432.0f, true, 0 };
    snap.activity     = { 85.0f,   true, 0 };
    float bv = battery_voltage();
    snap.battery_v    = { bv, bv > 0, 0 };
    ESP_LOGI(TAG, "battery: %.2f V", bv);

    display_lock(0);
    ui_dashboard_create();
    ui_dashboard_update(snap);
    display_unlock();

    ESP_LOGI(TAG, "dashboard built");

    // --- Milestone C: C6 link → connect → time sync → test HTTPS fetch ---
    if (net_init() == ESP_OK) {
        if (net_connect(WIFI_SSID, WIFI_PASS, 25000) == ESP_OK) {
            net_sntp_sync(15000);
            setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);   // Europe/London
            tzset();

            // Refresh Oura data on a timer, forever.
            const int REFRESH_SEC = 600;   // 10 minutes
            while (true) {
                oura_fetch(OURA_TOKEN, snap);
                withings_fetch(WITHINGS_CLIENT_ID, WITHINGS_CLIENT_SECRET, WITHINGS_REFRESH_TOKEN, snap);
                weather_fetch(snap);
                bv = battery_voltage();
                if (bv > 0) snap.battery_v = { bv, true, time(nullptr) };   // else keep last-good

                char status[32];
                time_t now = time(nullptr);
                struct tm tm_loc;
                localtime_r(&now, &tm_loc);
                strftime(status, sizeof status, "Updated %H:%M", &tm_loc);

                display_lock(0);
                ui_dashboard_update(snap);
                ui_dashboard_set_status(status);
                ui_dashboard_set_wifi(net_rssi());
                display_unlock();

                vTaskDelay(pdMS_TO_TICKS(REFRESH_SEC * 1000));
            }
        } else {
            ESP_LOGE(TAG, "Wi-Fi connect failed — set your password in main/wifi_credentials.h");
        }
    } else {
        ESP_LOGE(TAG, "net_init failed");
    }

    // Reached only on network failure — keep the (mock) dashboard up. (Returning
    // from app_main deletes the main task, whose cleanup overflows the idle stack.)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
