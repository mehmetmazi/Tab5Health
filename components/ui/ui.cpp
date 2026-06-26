/*
 * Dashboard UI: dark theme, a reusable metric card, and a grid of health tiles.
 * Pure LVGL — knows nothing about M5GFX or the data sources.
 */
#include "ui.hpp"
#include "lvgl.h"
#include <cstdio>
#include <ctime>

// ---- theme ----
static lv_color_t c_bg()   { return lv_color_hex(0x0E1116); }
static lv_color_t c_card() { return lv_color_hex(0x1A1F26); }
static lv_color_t c_text() { return lv_color_hex(0xF0F2F5); }
static lv_color_t c_dim()  { return lv_color_hex(0x8B94A3); }
static lv_color_t c_source(Source s)
{
    switch (s) {
        case Source::Oura:     return lv_color_hex(0x9B7BD4);   // purple
        case Source::Withings: return lv_color_hex(0x4FC3C7);   // teal
        case Source::Device:   return lv_color_hex(0x5BBF7B);   // green (on-device)
        case Source::Weather:  return lv_color_hex(0x6FA8DC);   // blue (Open-Meteo)
        default:               return lv_color_hex(0xF0B84B);   // Hilo amber
    }
}

enum { V_SLEEP, V_READY, V_HRV, V_RHR, V_SPO2, V_RESP, V_TEMP, V_STEPS, V_ACTIVITY,
       V_WEIGHT, V_FAT, V_MUSCLE, V_WATER, V_WEATHER, V_AQI, V_COUNT };
static lv_obj_t *s_val[V_COUNT];   // value labels, for in-place updates
static lv_obj_t *s_status;          // top-bar status / "updated" label
static lv_obj_t *s_clock;           // top-bar live clock
static lv_obj_t *s_age[V_COUNT];    // per-tile freshness label ("3d") when stale
static lv_obj_t *s_bat_fill;        // header battery icon: fill bar
static lv_obj_t *s_bat_pct;         // header battery icon: percentage
static lv_obj_t *s_wifi;            // header Wi-Fi signal glyph
static lv_obj_t *s_wx_cond;         // weather tile: condition text
static lv_obj_t *s_wx_range;        // weather tile: feels-like + today's hi/lo
static lv_obj_t *s_spark[V_COUNT];               // per-tile sparkline chart
static lv_chart_series_t *s_spark_ser[V_COUNT];  // its line series

// Health-status color for a metric value (green / amber / red), or neutral.
static lv_color_t metric_color(int i, float v)
{
    auto c = [](bool good, bool warn) {
        return good ? lv_color_hex(0x5BBF7B) : warn ? lv_color_hex(0xF0B84B) : lv_color_hex(0xE5705B);
    };
    switch (i) {
        case V_SLEEP:
        case V_READY:   return c(v >= 80, v >= 60);
        case V_HRV:     return c(v >= 60, v >= 40);
        case V_RHR:     return c(v <= 55, v <= 70);
        case V_SPO2:    return c(v >= 95, v >= 92);
        case V_RESP:    return c(v >= 12 && v <= 20, v >= 10 && v <= 24);
        case V_STEPS:    return c(v >= 8000, v >= 4000);
        case V_ACTIVITY: return c(v >= 80, v >= 60);
        case V_AQI:     return c(v <= 50, v <= 100);     // lower AQI is better
        default:        return lv_color_hex(0xF0F2F5);   // neutral (weight, fat, temp, …)
    }
}

// Is a rising value good? +1 higher-better, -1 lower-better, 0 neutral.
[[maybe_unused]] static int direction(int i)
{
    switch (i) {
        case V_SLEEP: case V_READY: case V_HRV:    case V_ACTIVITY:
        case V_SPO2:  case V_STEPS: case V_MUSCLE: return 1;
        case V_RHR:   case V_FAT:                  return -1;
        default:                                   return 0;
    }
}

// iPhone-style header battery: % text + a fill bar that shrinks and reddens as it
// drains. % is estimated from the NP-F550 (2S Li-ion) curve: ~6.6 V empty .. 8.4 V full.
static void set_battery_icon(const Metric &m)
{
    if (!s_bat_fill || !s_bat_pct) return;
    if (!m.valid) {
        lv_label_set_text(s_bat_pct, "--");
        lv_obj_set_width(s_bat_fill, 0);
        return;
    }
    int pct = (int)((m.value - 6.6f) / (8.4f - 6.6f) * 100.0f);
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    char b[8];
    snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(s_bat_pct, b);
    lv_obj_align(s_bat_pct, LV_ALIGN_TOP_RIGHT, -86, 28);
    int w = 38 * pct / 100;
    lv_obj_set_width(s_bat_fill, w < 2 ? 2 : w);
    lv_obj_set_style_bg_color(s_bat_fill, pct <= 20 ? lv_color_hex(0xE5705B) : lv_color_hex(0x5BBF7B), 0);
}

static void clock_cb(lv_timer_t *)
{
    if (!s_clock) return;
    time_t now = time(nullptr);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);
    char buf[48];
    strftime(buf, sizeof buf, "%H:%M   %a %d %b", &tm_loc);
    lv_label_set_text(s_clock, buf);
}

static lv_obj_t *make_card(lv_obj_t *parent, int idx, const char *title, const char *unit, Source src)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 236, 196);
    lv_obj_set_style_bg_color(card, c_card(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_set_size(accent, 44, 6);
    lv_obj_set_style_bg_color(accent, c_source(src), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 3, 0);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, c_dim(), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, c_text(), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, 8);

    lv_obj_t *u = lv_label_create(card);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, c_dim(), 0);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_align(u, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *age = lv_label_create(card);
    lv_label_set_text(age, "");
    lv_obj_set_style_text_color(age, c_dim(), 0);
    lv_obj_set_style_text_font(age, &lv_font_montserrat_14, 0);
    lv_obj_align(age, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_age[idx] = age;

    lv_obj_t *spark = lv_chart_create(card);
    lv_obj_set_size(spark, 128, 26);
    lv_obj_align(spark, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_chart_set_type(spark, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(spark, 0, 0);
    lv_chart_set_point_count(spark, METRIC_HIST);
    lv_obj_set_style_border_width(spark, 0, 0);
    lv_obj_set_style_bg_opa(spark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(spark, 0, 0);
    lv_obj_set_style_width(spark, 0, LV_PART_INDICATOR);    // hide point markers
    lv_obj_set_style_height(spark, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(spark, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(spark, LV_OBJ_FLAG_SCROLLABLE);
    s_spark_ser[idx] = lv_chart_add_series(spark, c_source(src), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_flag(spark, LV_OBJ_FLAG_HIDDEN);            // shown once history arrives
    s_spark[idx] = spark;

    return v;
}

// WMO weather code -> short condition label (Open-Meteo's current.weather_code).
static const char *wmo_text(int code)
{
    switch (code) {
        case 0:                             return "Clear";
        case 1: case 2:                     return "Partly cloudy";
        case 3:                             return "Overcast";
        case 45: case 48:                   return "Fog";
        case 51: case 53: case 55:
        case 56: case 57:                   return "Drizzle";
        case 61: case 63: case 65:
        case 66: case 67:                   return "Rain";
        case 71: case 73: case 75: case 77: return "Snow";
        case 80: case 81: case 82:          return "Showers";
        case 85: case 86:                   return "Snow showers";
        case 95: case 96: case 99:          return "Thunderstorm";
        default:                            return "--";
    }
}

// The weather tile is richer than a plain metric: temp + condition + feels/hi-lo.
static lv_obj_t *make_weather_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 236, 196);
    lv_obj_set_style_bg_color(card, c_card(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_set_size(accent, 44, 6);
    lv_obj_set_style_bg_color(accent, c_source(Source::Weather), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 3, 0);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "WEATHER");
    lv_obj_set_style_text_color(t, c_dim(), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, c_text(), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, -8);

    s_wx_cond = lv_label_create(card);
    lv_label_set_text(s_wx_cond, "");
    lv_obj_set_style_text_color(s_wx_cond, c_text(), 0);
    lv_obj_set_style_text_font(s_wx_cond, &lv_font_montserrat_14, 0);
    lv_obj_align(s_wx_cond, LV_ALIGN_BOTTOM_LEFT, 0, -20);

    s_wx_range = lv_label_create(card);
    lv_label_set_text(s_wx_range, "");
    lv_obj_set_style_text_color(s_wx_range, c_dim(), 0);
    lv_obj_set_style_text_font(s_wx_range, &lv_font_montserrat_14, 0);
    lv_obj_align(s_wx_range, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_age[V_WEATHER] = nullptr;   // weather is always fresh — no age label
    return v;
}

void ui_dashboard_create()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, c_bg(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- top bar ----
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Health");
    lv_obj_set_style_text_color(title, c_text(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 22);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "connecting");
    lv_obj_set_style_text_color(s_status, c_dim(), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 30, 56);

    s_clock = lv_label_create(scr);
    lv_obj_set_style_text_color(s_clock, c_text(), 0);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_28, 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 22);
    clock_cb(nullptr);
    lv_timer_create(clock_cb, 30000, nullptr);   // refresh the clock every 30 s

    // ---- iPhone-style battery icon (top-right) ----
    lv_obj_t *bat = lv_obj_create(scr);
    lv_obj_set_size(bat, 48, 24);
    lv_obj_set_style_bg_opa(bat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bat, 2, 0);
    lv_obj_set_style_border_color(bat, c_dim(), 0);
    lv_obj_set_style_radius(bat, 5, 0);
    lv_obj_set_style_pad_all(bat, 2, 0);
    lv_obj_clear_flag(bat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bat, LV_ALIGN_TOP_RIGHT, -30, 26);

    lv_obj_t *nub = lv_obj_create(scr);
    lv_obj_set_size(nub, 5, 10);
    lv_obj_set_style_bg_color(nub, c_dim(), 0);
    lv_obj_set_style_border_width(nub, 0, 0);
    lv_obj_set_style_radius(nub, 2, 0);
    lv_obj_align(nub, LV_ALIGN_TOP_RIGHT, -25, 33);

    s_bat_fill = lv_obj_create(bat);
    lv_obj_set_size(s_bat_fill, 0, 16);
    lv_obj_set_style_border_width(s_bat_fill, 0, 0);
    lv_obj_set_style_radius(s_bat_fill, 2, 0);
    lv_obj_set_style_bg_color(s_bat_fill, c_dim(), 0);
    lv_obj_clear_flag(s_bat_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_bat_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_bat_pct = lv_label_create(scr);
    lv_label_set_text(s_bat_pct, "--");
    lv_obj_set_style_text_color(s_bat_pct, c_text(), 0);
    lv_obj_set_style_text_font(s_bat_pct, &lv_font_montserrat_14, 0);
    lv_obj_align(s_bat_pct, LV_ALIGN_TOP_RIGHT, -86, 28);

    s_wifi = lv_label_create(scr);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, c_dim(), 0);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -140, 28);

    // ---- card grid (5 x 3) ----
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, 1280, 648);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 12, 0);
    lv_obj_set_style_pad_gap(grid, 12, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_val[V_SLEEP]   = make_card(grid, V_SLEEP,   "SLEEP",       "/ 100", Source::Oura);
    s_val[V_READY]   = make_card(grid, V_READY,   "READINESS",   "/ 100", Source::Oura);
    s_val[V_HRV]     = make_card(grid, V_HRV,     "HRV",         "ms",    Source::Oura);
    s_val[V_RHR]     = make_card(grid, V_RHR,     "RESTING HR",  "bpm",   Source::Oura);
    s_val[V_SPO2]    = make_card(grid, V_SPO2,    "SpO2",        "%",     Source::Oura);
    s_val[V_RESP]    = make_card(grid, V_RESP,    "RESP RATE",   "/min",  Source::Oura);
    s_val[V_TEMP]    = make_card(grid, V_TEMP,    "BODY TEMP",   "C dev", Source::Oura);
    s_val[V_STEPS]   = make_card(grid, V_STEPS,   "STEPS",       "today", Source::Oura);
    s_val[V_ACTIVITY]= make_card(grid, V_ACTIVITY,"ACTIVITY",    "/ 100", Source::Oura);
    s_val[V_WEIGHT]  = make_card(grid, V_WEIGHT,  "WEIGHT",      "kg",    Source::Withings);
    s_val[V_FAT]     = make_card(grid, V_FAT,     "BODY FAT",    "%",     Source::Withings);
    s_val[V_MUSCLE]  = make_card(grid, V_MUSCLE,  "MUSCLE",      "kg",    Source::Withings);
    s_val[V_WATER]   = make_card(grid, V_WATER,   "WATER",       "%",     Source::Withings);
    s_val[V_WEATHER] = make_weather_card(grid);
    s_val[V_AQI]     = make_card(grid, V_AQI,     "AIR QUALITY", "AQI",   Source::Weather);
}

void ui_dashboard_set_status(const char *text)
{
    if (!s_status) return;
    lv_label_set_text(s_status, text);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 30, 56);   // re-anchor (under the title)
}

void ui_dashboard_set_wifi(int rssi)
{
    if (!s_wifi) return;
    lv_color_t col = rssi == 0   ? c_dim()              // disconnected
                   : rssi > -60  ? lv_color_hex(0x5BBF7B)
                   : rssi > -72  ? lv_color_hex(0xF0B84B)
                                 : lv_color_hex(0xE5705B);
    lv_obj_set_style_text_color(s_wifi, col, 0);
}

// Draw a tile's sparkline from its history (scaled ×10 to keep one decimal).
static void set_spark(int i, const Metric &m)
{
    if (!s_spark[i]) return;
    if (m.hist_n < 2) { lv_obj_add_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN); return; }
    float mn = m.hist[0], mx = m.hist[0];
    for (int k = 1; k < m.hist_n; k++) {
        if (m.hist[k] < mn) mn = m.hist[k];
        if (m.hist[k] > mx) mx = m.hist[k];
    }
    if (mx - mn < 0.001f) { mn -= 1; mx += 1; }   // flat series -> avoid a zero range
    lv_chart_set_point_count(s_spark[i], m.hist_n);
    lv_chart_set_range(s_spark[i], LV_CHART_AXIS_PRIMARY_Y,
                       (int32_t)(mn * 10), (int32_t)(mx * 10));
    for (int k = 0; k < m.hist_n; k++)
        lv_chart_set_value_by_id(s_spark[i], s_spark_ser[i], k, (int32_t)(m.hist[k] * 10));
    lv_chart_refresh(s_spark[i]);
    lv_obj_clear_flag(s_spark[i], LV_OBJ_FLAG_HIDDEN);
}

static void set_v(int i, const Metric &m, const char *fmt)
{
    char b[24];
    if (m.valid) snprintf(b, sizeof b, fmt, m.value);
    else         snprintf(b, sizeof b, "--");
    lv_label_set_text(s_val[i], b);

    // Freshness: data older than ~2 days greys out and shows its age ("3d").
    time_t now = time(nullptr);
    bool stale = m.valid && m.updated > 0 && (now - m.updated) >= 48 * 3600;
    lv_obj_set_style_text_color(s_val[i],
        (!m.valid || stale) ? c_dim() : metric_color(i, m.value), 0);

    if (s_age[i]) {
        if (stale) {
            char a[8];
            snprintf(a, sizeof a, "%dd", (int)((now - m.updated) / 86400));
            lv_label_set_text(s_age[i], a);
        } else {
            lv_label_set_text(s_age[i], "");
        }
        lv_obj_align(s_age[i], LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    set_spark(i, m);
}

void ui_dashboard_update(const HealthSnapshot &s)
{
    set_v(V_SLEEP,   s.sleep_score,  "%.0f");
    set_v(V_READY,   s.readiness,    "%.0f");
    set_v(V_HRV,     s.hrv_ms,       "%.0f");
    set_v(V_RHR,     s.resting_hr,   "%.0f");
    set_v(V_SPO2,    s.spo2,         "%.0f");
    set_v(V_RESP,    s.resp_rate,    "%.0f");
    set_v(V_TEMP,    s.body_temp,    "%+.1f");
    set_v(V_STEPS,   s.steps,        "%.0f");
    set_v(V_ACTIVITY,s.activity,     "%.0f");
    set_v(V_WEIGHT,  s.weight_kg,    "%.1f");
    set_v(V_FAT,     s.body_fat_pct, "%.1f");
    set_v(V_MUSCLE,  s.muscle_kg,    "%.1f");
    set_v(V_WATER,   s.water_pct,    "%.1f");
    set_v(V_WEATHER, s.weather_temp, "%.0f");
    if (s_wx_cond) lv_label_set_text(s_wx_cond, wmo_text(s.weather_code));
    if (s_wx_range && s.temp_hi.valid) {
        char r[48];
        snprintf(r, sizeof r, "feels %.0f  H%.0f L%.0f",
                 s.feels_like.value, s.temp_hi.value, s.temp_lo.value);
        lv_label_set_text(s_wx_range, r);
    }
    set_v(V_AQI,     s.aqi,          "%.0f");
    set_battery_icon(s.battery_v);
}
