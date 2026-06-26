/*
 * Oura provider — fetches the v2 user-collection API with a Personal Access Token
 * (Bearer auth) over the C6/HTTPS link, parses JSON (cJSON), and fills the
 * Oura-sourced HealthSnapshot fields.
 *
 *   sleep_score <- daily_sleep.score
 *   readiness   <- daily_readiness.score
 *   hrv_ms      <- sleep.average_hrv
 *   resting_hr  <- sleep.lowest_heart_rate
 */
#include "oura.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

static const char *TAG = "oura";

struct resp_buf {
    char *data;
    int len;
    int cap;
};

static esp_err_t on_data(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        auto *b = static_cast<resp_buf *>(e->user_data);
        int n = e->data_len;
        if (b->len + n > b->cap - 1) n = b->cap - 1 - b->len;   // leave room for NUL
        if (n > 0) {
            memcpy(b->data + b->len, e->data, n);
            b->len += n;
        }
    }
    return ESP_OK;
}

// GET https://api.ouraring.com/v2/usercollection/<path>?start_date&end_date  (Bearer auth)
static esp_err_t oura_get(const char *token, const char *path,
                          const char *start, const char *end, char *buf, int cap)
{
    char url[224];
    snprintf(url, sizeof url,
             "https://api.ouraring.com/v2/usercollection/%s?start_date=%s&end_date=%s",
             path, start, end);

    resp_buf rb { buf, 0, cap };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.event_handler = on_data;
    cfg.user_data = &rb;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char auth[96];
    snprintf(auth, sizeof auth, "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    buf[rb.len] = 0;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", path, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "%s: HTTP %d: %.120s", path, status, buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s (%d B): %.320s", path, rb.len, buf);   // raw, for field verification
    return ESP_OK;
}

// Parse "YYYY-MM-DD" -> time_t at the END of that day (next midnight, UTC). The
// daily value is finalised the next morning, so stamping end-of-day keeps a normal
// "yesterday" reading fresh (~0-24h) instead of a misleading ~36h.
static time_t day_end_ts(const char *day)
{
    int y, m, d;
    if (!day || sscanf(day, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    // days since 1970-01-01 (Howard Hinnant's civil->days; newlib lacks timegm)
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return ((time_t)days + 1) * 86400;   // +1 day = end-of-day (next midnight UTC)
}

// Recent history of one field from an Oura data[] response. Value + day come from
// the SAME (newest non-null) element; null / incomplete days are skipped.
struct DaySeries {
    float  v = 0; bool ok = false; time_t when = 0;
    float  hist[METRIC_HIST]; int n = 0;
};

// Walk data[] ascending; pull data[].<outer> (or nested data[].<outer>.<inner>),
// keeping the last METRIC_HIST valid values + the newest valid element's day.
static void extract(const char *json, const char *outer, const char *inner, DaySeries &s)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        int cnt = cJSON_GetArraySize(data);
        for (int i = 0; i < cnt; i++) {
            cJSON *el = cJSON_GetArrayItem(data, i);
            cJSON *o  = cJSON_GetObjectItem(el, outer);
            cJSON *f  = inner ? cJSON_GetObjectItem(o, inner) : o;
            if (!cJSON_IsNumber(f)) continue;                 // skip null / incomplete day
            float val = (float)f->valuedouble;
            if (s.n < METRIC_HIST) s.hist[s.n++] = val;
            else { memmove(s.hist, s.hist + 1, (METRIC_HIST - 1) * sizeof(float));
                   s.hist[METRIC_HIST - 1] = val; }
            s.v = val; s.ok = true;
            cJSON *day = cJSON_GetObjectItem(el, "day");
            if (cJSON_IsString(day)) s.when = day_end_ts(day->valuestring);
        }
    }
    cJSON_Delete(root);
}

// Fill a Metric from the newest valid value of an Oura field: value, data-date
// (for freshness), day-over-day trend, and the sparkline history.
static void set_daily(Metric &m, const char *json, const char *outer, const char *inner = nullptr)
{
    DaySeries s;
    extract(json, outer, inner, s);
    if (!s.ok) return;
    m.value   = s.v;
    m.valid   = true;
    m.updated = s.when;
    m.hist_n  = s.n;
    for (int i = 0; i < s.n; i++) m.hist[i] = s.hist[i];
    m.trend   = s.n >= 2 ? (s.hist[s.n-1] > s.hist[s.n-2] ? 1
                          : s.hist[s.n-1] < s.hist[s.n-2] ? -1 : 0) : 0;
}

esp_err_t oura_fetch(const char *token, HealthSnapshot &snap)
{
    if (!token || !token[0]) {
        ESP_LOGE(TAG, "no Oura token — set main/oura_credentials.h");
        return ESP_ERR_INVALID_ARG;
    }

    // Date window: last 3 days (UTC). Oura returns data[] ascending by day; we take
    // the most recent valid value, so an incomplete "today" is skipped gracefully.
    time_t now    = time(nullptr);
    time_t future = now + 24 * 3600;        // tomorrow: Oura omits the current day unless end_date is ahead of it
    time_t past   = now - 5 * 24 * 3600;    // ~6-day window so the sparklines get several points
    struct tm tm_e, tm_s;
    gmtime_r(&future, &tm_e);
    gmtime_r(&past, &tm_s);
    char end[16], start[16];
    strftime(end, sizeof end, "%Y-%m-%d", &tm_e);
    strftime(start, sizeof start, "%Y-%m-%d", &tm_s);
    ESP_LOGI(TAG, "fetching %s … %s", start, end);

    // daily_activity carries a per-day 5-min MET series (tens of KB) and the sleep
    // endpoint HR/HRV series; size the PSRAM buffer so neither truncates over the window.
    const int CAP = 96 * 1024;
    char *buf = static_cast<char *>(heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM));
    if (!buf) return ESP_ERR_NO_MEM;

    if (oura_get(token, "daily_sleep", start, end, buf, CAP) == ESP_OK)
        set_daily(snap.sleep_score, buf, "score");

    if (oura_get(token, "daily_readiness", start, end, buf, CAP) == ESP_OK) {
        set_daily(snap.readiness, buf, "score");
        set_daily(snap.body_temp, buf, "temperature_deviation");
    }

    if (oura_get(token, "sleep", start, end, buf, CAP) == ESP_OK) {
        set_daily(snap.hrv_ms,     buf, "average_hrv");
        set_daily(snap.resting_hr, buf, "lowest_heart_rate");
        set_daily(snap.resp_rate,  buf, "average_breath");
    }

    if (oura_get(token, "daily_spo2", start, end, buf, CAP) == ESP_OK)
        set_daily(snap.spo2, buf, "spo2_percentage", "average");

    if (oura_get(token, "daily_activity", start, end, buf, CAP) == ESP_OK) {
        set_daily(snap.steps,    buf, "steps");
        set_daily(snap.activity, buf, "score");
    }

    free(buf);
    ESP_LOGI(TAG, "sleep=%.0f ready=%.0f hrv=%.0f rhr=%.0f spo2=%.0f resp=%.0f temp=%.1f steps=%.0f act=%.0f",
             snap.sleep_score.value, snap.readiness.value, snap.hrv_ms.value,
             snap.resting_hr.value, snap.spo2.value, snap.resp_rate.value,
             snap.body_temp.value, snap.steps.value, snap.activity.value);
    return ESP_OK;
}
