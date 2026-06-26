#pragma once
#include "model.hpp"

// Builds the dashboard on LVGL's active screen. Call with the display lock held.
void ui_dashboard_create();

// Pushes a snapshot into the dashboard tiles. Call with the display lock held.
void ui_dashboard_update(const HealthSnapshot &snap);

// Sets the top-bar status text (e.g. "Updated 11:34"). Call with the lock held.
void ui_dashboard_set_status(const char *text);

// Updates the header Wi-Fi icon from RSSI (dBm; 0 = disconnected). Lock held.
void ui_dashboard_set_wifi(int rssi);
