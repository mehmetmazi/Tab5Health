#pragma once
#include "esp_err.h"
#include "model.hpp"

// Fetch the latest Withings weight + body-fat over the C6/HTTPS link and fill
// snap.weight_kg + snap.body_fat_pct. Uses an OAuth2 refresh token, which Withings
// ROTATES on every refresh — the rotated token is persisted in NVS and seeded from
// seed_refresh_token on first run. Requires SNTP time + nvs_flash_init done.
esp_err_t withings_fetch(const char *client_id, const char *client_secret,
                         const char *seed_refresh_token, HealthSnapshot &snap);
