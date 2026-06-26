#pragma once
#include "esp_err.h"
#include "model.hpp"

// Fetch the latest Oura metrics (over the C6/HTTPS link) and fill the Oura-sourced
// fields of `snap`: sleep_score, readiness, hrv_ms, resting_hr. Other fields are
// left untouched. Requires SNTP time to be set (for the date window + TLS).
esp_err_t oura_fetch(const char *token, HealthSnapshot &snap);
