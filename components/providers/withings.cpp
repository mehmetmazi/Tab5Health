/*
 * Withings provider — OAuth2 (refresh-token grant) over the C6/HTTPS link.
 *   - refresh: POST /v2/oauth2 (grant_type=refresh_token) -> access_token (+ a
 *     ROTATED refresh_token, which we persist to NVS so the next boot still works).
 *   - data:    POST /measure (action=getmeas) -> measuregrps -> weight + fat ratio.
 *
 *   weight_kg     <- measure type 1
 *   body_fat_pct  <- measure type 6
 */
#include "withings.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>

static const char *TAG = "withings";

static char   s_access[320];     // cached access token (valid ~3h)
static time_t s_access_exp = 0;

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

// POST a form body; optional Bearer token. Returns HTTP status (-1 on transport err).
static int http_post(const char *url, const char *bearer, const char *post, char *buf, int cap)
{
    rbuf rb { buf, 0, cap };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 12000;
    cfg.event_handler = on_data;
    cfg.user_data = &rb;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    if (bearer) {
        char a[360];
        snprintf(a, sizeof a, "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", a);
    }
    esp_http_client_set_post_field(c, post, strlen(post));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    buf[rb.len] = 0;
    if (err != ESP_OK) { ESP_LOGE(TAG, "POST %s: %s", url, esp_err_to_name(err)); return -1; }
    return status;
}

// Withings rotates the refresh token on every refresh — keep the latest in NVS.
static bool nvs_get_rt(char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("withings", NVS_READWRITE, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t r = nvs_get_str(h, "rt", out, &len);
    nvs_close(h);
    return r == ESP_OK && len > 1;
}

static void nvs_set_rt(const char *rt)
{
    nvs_handle_t h;
    if (nvs_open("withings", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "rt", rt);
    nvs_commit(h);
    nvs_close(h);
}

static int json_status(cJSON *root)
{
    cJSON *s = cJSON_GetObjectItem(root, "status");
    return cJSON_IsNumber(s) ? s->valueint : -1;
}

// Exchange the (NVS or seed) refresh token for a fresh access token; persist the rotated rt.
static bool refresh(const char *cid, const char *secret, const char *seed_rt, char *buf, int cap)
{
    char rt[320];
    if (!nvs_get_rt(rt, sizeof rt)) { strncpy(rt, seed_rt, sizeof rt - 1); rt[sizeof rt - 1] = 0; }

    char post[900];
    snprintf(post, sizeof post,
             "action=requesttoken&grant_type=refresh_token"
             "&client_id=%s&client_secret=%s&refresh_token=%s", cid, secret, rt);

    int st = http_post("https://wbsapi.withings.net/v2/oauth2", nullptr, post, buf, cap);
    if (st != 200) { ESP_LOGE(TAG, "refresh HTTP %d", st); return false; }

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    bool ok = false;
    if (json_status(root) == 0) {
        cJSON *b   = cJSON_GetObjectItem(root, "body");
        cJSON *at  = cJSON_GetObjectItem(b, "access_token");
        cJSON *nrt = cJSON_GetObjectItem(b, "refresh_token");
        cJSON *ex  = cJSON_GetObjectItem(b, "expires_in");
        if (cJSON_IsString(at)) {
            strncpy(s_access, at->valuestring, sizeof s_access - 1);
            s_access[sizeof s_access - 1] = 0;
            s_access_exp = time(nullptr) + (cJSON_IsNumber(ex) ? ex->valueint : 10000);
            ok = true;
        }
        if (cJSON_IsString(nrt)) nvs_set_rt(nrt->valuestring);   // persist rotated token
    } else {
        ESP_LOGE(TAG, "refresh status=%d: %.140s", json_status(root), buf);
    }
    cJSON_Delete(root);
    return ok;
}

// Build a measure type's sparkline history (last METRIC_HIST readings, chronological).
// Re-scans the already-parsed groups; leaves value/valid/updated (set above) intact.
static void fill_hist(Metric &m, cJSON *grps, int type)
{
    if (!m.valid) return;
    struct Pt { double date; float val; };
    const int MAXP = 48;
    Pt p[MAXP]; int n = 0;
    cJSON *g;
    cJSON_ArrayForEach(g, grps) {
        cJSON *dt = cJSON_GetObjectItem(g, "date");
        double date = cJSON_IsNumber(dt) ? dt->valuedouble : 0;
        cJSON *meas = cJSON_GetObjectItem(g, "measures"), *mm;
        cJSON_ArrayForEach(mm, meas) {
            cJSON *tt = cJSON_GetObjectItem(mm, "type");
            cJSON *vv = cJSON_GetObjectItem(mm, "value");
            cJSON *uu = cJSON_GetObjectItem(mm, "unit");
            if (cJSON_IsNumber(tt) && tt->valueint == type &&
                cJSON_IsNumber(vv) && cJSON_IsNumber(uu) && n < MAXP)
                p[n++] = { date, (float)(vv->valuedouble * pow(10, uu->valueint)) };
        }
    }
    if (n < 2) return;
    for (int i = 1; i < n; i++) {                 // insertion sort, ascending by date
        Pt k = p[i]; int j = i - 1;
        while (j >= 0 && p[j].date > k.date) { p[j + 1] = p[j]; j--; }
        p[j + 1] = k;
    }
    int start = n > METRIC_HIST ? n - METRIC_HIST : 0;
    m.hist_n = n - start;
    for (int i = start; i < n; i++) m.hist[i - start] = p[i].val;
    m.trend = m.hist_n >= 2 ? (m.hist[m.hist_n-1] > m.hist[m.hist_n-2] ? 1
                            :  m.hist[m.hist_n-1] < m.hist[m.hist_n-2] ? -1 : 0) : 0;
}

esp_err_t withings_fetch(const char *cid, const char *secret, const char *seed_rt, HealthSnapshot &snap)
{
    if (!cid || !cid[0] || !seed_rt || !seed_rt[0]) {
        ESP_LOGW(TAG, "no Withings creds — set main/withings_credentials.h");
        return ESP_ERR_INVALID_ARG;
    }

    const int CAP = 32 * 1024;
    char *buf = static_cast<char *>(heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM));
    if (!buf) return ESP_ERR_NO_MEM;

    if (time(nullptr) > s_access_exp - 60) {
        if (!refresh(cid, secret, seed_rt, buf, CAP)) { free(buf); return ESP_FAIL; }
    }

    // getmeas: weight (type 1) + fat ratio (type 6), last 45 days.
    time_t now = time(nullptr), start = now - 45 * 24 * 3600;
    char post[160];
    snprintf(post, sizeof post,
             "action=getmeas&meastypes=1,6,76,77&category=1&startdate=%ld&enddate=%ld",
             (long)start, (long)now);
    int st = http_post("https://wbsapi.withings.net/measure", s_access, post, buf, CAP);
    if (st != 200) { ESP_LOGE(TAG, "getmeas HTTP %d", st); free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *grps = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "body"), "measuregrps");
        double bw = -1, bf = -1, bm = -1, bh = -1;
        double weight = 0, fat = 0, muscle = 0, water = 0;
        cJSON *g;
        cJSON_ArrayForEach(g, grps) {
            cJSON *dt = cJSON_GetObjectItem(g, "date");
            double date = cJSON_IsNumber(dt) ? dt->valuedouble : 0;
            cJSON *meas = cJSON_GetObjectItem(g, "measures"), *m;
            cJSON_ArrayForEach(m, meas) {
                cJSON *tt = cJSON_GetObjectItem(m, "type");
                cJSON *vv = cJSON_GetObjectItem(m, "value");
                cJSON *uu = cJSON_GetObjectItem(m, "unit");
                if (!cJSON_IsNumber(tt) || !cJSON_IsNumber(vv) || !cJSON_IsNumber(uu)) continue;
                double v = vv->valuedouble * pow(10, uu->valueint);
                if (tt->valueint == 1  && date > bw) { weight = v; bw = date; }   // weight kg
                if (tt->valueint == 6  && date > bf) { fat = v;    bf = date; }   // fat ratio %
                if (tt->valueint == 76 && date > bm) { muscle = v; bm = date; }   // muscle mass kg
                if (tt->valueint == 77 && date > bh) { water = v;  bh = date; }   // hydration kg
            }
        }
        if (bw >= 0) snap.weight_kg    = { (float)weight, true, (time_t)bw };   // stamp the measure date
        if (bf >= 0) snap.body_fat_pct = { (float)fat,    true, (time_t)bf };
        if (bm >= 0) snap.muscle_kg    = { (float)muscle, true, (time_t)bm };
        if (bh >= 0 && weight > 0)                                               // hydration -> % of body weight
            snap.water_pct = { (float)(water / weight * 100.0), true, (time_t)bh };
        fill_hist(snap.weight_kg,    grps, 1);     // sparkline history (weight / fat / muscle)
        fill_hist(snap.body_fat_pct, grps, 6);
        fill_hist(snap.muscle_kg,    grps, 76);
        ESP_LOGI(TAG, "weight=%.1f fat=%.1f muscle=%.1f water=%.0f%%",
                 weight, fat, muscle, weight > 0 ? water / weight * 100.0 : 0.0);
        cJSON_Delete(root);
    }

    free(buf);
    return ESP_OK;
}
