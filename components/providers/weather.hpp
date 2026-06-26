#pragma once
#include "esp_err.h"
#include "model.hpp"

// Fetch current outdoor temperature + air quality (Open-Meteo — free, no API key)
// for a fixed location and fill snap.weather_temp + snap.aqi.
esp_err_t weather_fetch(HealthSnapshot &snap);
