/*
 * Weather + air quality via Open-Meteo (free, no API key, no OAuth). Current
 * temperature + European AQI for a fixed location, over the C6/HTTPS link.
 */
#include "weather.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <ctime>

static const char *TAG = "weather";

// Location (defaults to central London). Set to your own coordinates.
static constexpr const char *LAT = "51.51";
static constexpr const char *LON = "-0.13";

struct rbuf { char *d; int len; int cap; };

static esp_err_t on_data(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        auto *b = static_cast<rbuf *>(e->user_data);
        int n = e->data_len;
        if (b->len + n > b->cap - 1) n = b->cap - 1 - b->len;
        if (n > 0) { memcpy(b->d + b->len, e->data, n); b->len += n; }
    }
    return ESP_OK;
}

static bool get_json(const char *url, char *buf, int cap)
{
    rbuf rb { buf, 0, cap };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.event_handler = on_data;
    cfg.user_data = &rb;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    buf[rb.len] = 0;
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "GET failed: err=%s http=%d", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

// Extract a numeric `current.<field>` from an Open-Meteo response.
static bool current_number(const char *json, const char *field, float *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *f = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "current"), field);
    bool ok = cJSON_IsNumber(f);
    if (ok) *out = (float)f->valuedouble;
    cJSON_Delete(root);
    return ok;
}

esp_err_t weather_fetch(HealthSnapshot &snap)
{
    char buf[2560], url[256];
    time_t now = time(nullptr);
    float v;

    // One forecast call: current temp + feels-like + condition, plus today's hi/lo.
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,apparent_temperature,weather_code"
             "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
             LAT, LON);
    if (get_json(url, buf, sizeof buf)) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *cur = cJSON_GetObjectItem(root, "current");
            cJSON *dly = cJSON_GetObjectItem(root, "daily");
            cJSON *tt  = cJSON_GetObjectItem(cur, "temperature_2m");
            cJSON *ap  = cJSON_GetObjectItem(cur, "apparent_temperature");
            cJSON *wc  = cJSON_GetObjectItem(cur, "weather_code");
            cJSON *hi  = cJSON_GetArrayItem(cJSON_GetObjectItem(dly, "temperature_2m_max"), 0);
            cJSON *lo  = cJSON_GetArrayItem(cJSON_GetObjectItem(dly, "temperature_2m_min"), 0);
            if (cJSON_IsNumber(tt)) snap.weather_temp = { (float)tt->valuedouble, true, now };
            if (cJSON_IsNumber(ap)) snap.feels_like   = { (float)ap->valuedouble, true, now };
            if (cJSON_IsNumber(wc)) snap.weather_code = wc->valueint;
            if (cJSON_IsNumber(hi)) snap.temp_hi      = { (float)hi->valuedouble, true, now };
            if (cJSON_IsNumber(lo)) snap.temp_lo      = { (float)lo->valuedouble, true, now };
            cJSON_Delete(root);
        }
    }

    snprintf(url, sizeof url,
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s&current=european_aqi",
             LAT, LON);
    if (get_json(url, buf, sizeof buf) && current_number(buf, "european_aqi", &v))
        snap.aqi = { v, true, now };

    ESP_LOGI(TAG, "temp=%.0f feels=%.0f code=%d hi=%.0f lo=%.0f aqi=%.0f",
             snap.weather_temp.value, snap.feels_like.value, snap.weather_code,
             snap.temp_hi.value, snap.temp_lo.value, snap.aqi.value);
    return ESP_OK;
}
