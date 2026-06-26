#pragma once
#include <ctime>

// One health metric with freshness metadata. `valid == false` means "no data
// yet" (render as "--"); `updated` is the time of the last successful fetch.
static constexpr int METRIC_HIST = 7;   // sparkline series length

struct Metric {
    float  value   = 0.0f;
    bool   valid   = false;
    time_t updated = 0;              // data timestamp (not fetch time) — drives freshness
    int    trend   = 0;              // vs previous: -1 down · 0 none · +1 up
    float  hist[METRIC_HIST] = {};   // recent values oldest..newest — the sparkline
    int    hist_n  = 0;
};

// Which service a metric came from — drives the accent color in the UI.
enum class Source { Oura, Withings, Hilo, Device, Weather };

// The full set of metrics shown on the dashboard. Providers fill this in; the UI
// reads it. Deliberately decoupled from both so each can evolve independently.
struct HealthSnapshot {
    Metric sleep_score;    // Oura,       0-100
    Metric readiness;      // Oura,       0-100
    Metric hrv_ms;         // Oura,       ms
    Metric resting_hr;     // Oura,       bpm
    Metric spo2;           // Oura,       %
    Metric resp_rate;      // Oura,       breaths/min
    Metric body_temp;      // Oura,       °C deviation
    Metric steps;          // Oura,       count
    Metric activity;       // Oura,       0-100 (activity score)
    Metric weight_kg;      // Withings,   kg
    Metric body_fat_pct;   // Withings,   %
    Metric muscle_kg;      // Withings,   kg
    Metric water_pct;      // Withings,   %
    Metric weather_temp;   // Open-Meteo, °C
    Metric feels_like;     // Open-Meteo, °C (apparent)
    Metric temp_hi;        // Open-Meteo, °C (today's max)
    Metric temp_lo;        // Open-Meteo, °C (today's min)
    int    weather_code = -1;  // Open-Meteo WMO weather code
    Metric aqi;            // Open-Meteo, AQI
    Metric battery_v;      // Device,     V (INA226 battery monitor)
};
