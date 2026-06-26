#pragma once
#include "esp_err.h"
#include <cstdint>

// Brings up esp-hosted (SDIO link to the ESP32-C6) + the Wi-Fi STA stack.
esp_err_t net_init();

// Blocking Wi-Fi scan that logs visible APs (validates the C6 transport).
void net_scan();

// Connect to an AP (STA). Blocks until an IP is obtained or timeout elapses.
esp_err_t net_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// SNTP time sync (needed for TLS cert validation + the RTC). Blocks until set.
esp_err_t net_sntp_sync(uint32_t timeout_ms);

// Test HTTPS GET (validates TLS over the C6). Logs status code + body.
esp_err_t net_https_get(const char *url);

// Current STA signal strength (RSSI in dBm, negative). Returns 0 if disconnected.
int net_rssi();
