/*
 * Networking: P4 host <-> ESP32-C6 over esp-hosted (SDIO). esp_wifi_remote routes
 * the standard esp_wifi_* API to the C6. SDIO pins/transport are set in sdkconfig.
 */
#include "net.hpp"
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <cstring>
#include <ctime>

static const char *TAG = "net";
static EventGroupHandle_t s_events;
static const int GOT_IP_BIT = BIT0;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

esp_err_t net_init()
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(r, TAG, "nvs");

    ESP_LOGI(TAG, "esp_hosted_init (SDIO link to C6)…");
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_event, nullptr, nullptr),
                        TAG, "reg wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_event, nullptr, nullptr),
                        TAG, "reg ip evt");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");
    ESP_LOGI(TAG, "Wi-Fi STA started (C6 link up)");
    return ESP_OK;
}

void net_scan()
{
    ESP_LOGI(TAG, "scanning for APs…");
    wifi_scan_config_t sc = {};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGE(TAG, "scan failed (C6 link?)");
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "found %u AP(s):", n);
    uint16_t got = n > 20 ? 20 : n;
    wifi_ap_record_t recs[20];
    if (got && esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (int i = 0; i < got; i++) {
            ESP_LOGI(TAG, "  %-32s  rssi=%4d  ch=%d",
                     (const char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
        }
    }
}

int net_rssi()
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

esp_err_t net_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config");

    xEventGroupClearBits(s_events, GOT_IP_BIT);
    ESP_LOGI(TAG, "connecting to \"%s\"…", ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");

    EventBits_t b = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t net_sntp_sync(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "SNTP sync (pool.ntp.org)…");
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&cfg), TAG, "sntp_init");

    esp_err_t r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (r == ESP_OK) {
        time_t now = time(nullptr);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        char buf[32];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm_utc);
        ESP_LOGI(TAG, "time synced: %s UTC", buf);
    } else {
        ESP_LOGW(TAG, "SNTP sync timed out");
    }
    return r;
}

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        int n = e->data_len > 160 ? 160 : e->data_len;
        ESP_LOGI(TAG, "  body: %.*s", n, (const char *)e->data);
    }
    return ESP_OK;
}

esp_err_t net_https_get(const char *url)
{
    ESP_LOGI(TAG, "HTTPS GET %s", url);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // validate against the CA bundle
    cfg.timeout_ms = 12000;
    cfg.event_handler = http_evt;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS ok: status=%d len=%lld",
                 esp_http_client_get_status_code(c),
                 (long long)esp_http_client_get_content_length(c));
    } else {
        ESP_LOGE(TAG, "HTTPS failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    return err;
}
