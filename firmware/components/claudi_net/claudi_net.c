// claudi_net.c — see claudi_net.h.
#include "claudi_net.h"

#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

// Wi-Fi credentials. Defaults match the existing project's board_config.h; you
// can override at build time with -DCLAUDI_WIFI_SSID=\"...\" etc.
#ifndef CLAUDI_WIFI_SSID
#define CLAUDI_WIFI_SSID "PPHome"
#endif
#ifndef CLAUDI_WIFI_PASS
#define CLAUDI_WIFI_PASS "19897913"
#endif
#ifndef CLAUDI_MDNS_HOST
#define CLAUDI_MDNS_HOST "claudi"  // → claudi.local
#endif

#define CLAUDI_SNAPSHOT_BODY_MAX 4096

static const char *TAG = "claudi_net";

static claudi_snapshot_t s_snap;
static SemaphoreHandle_t s_mtx;
static volatile bool s_connected;
static char s_ip[16] = "0.0.0.0";
static httpd_handle_t s_httpd;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
}

void claudi_net_get_snapshot(claudi_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = s_snap;
    unlock();
}

bool claudi_net_is_connected(void)
{
    return s_connected;
}

const char *claudi_net_ip(void)
{
    return s_ip;
}

// --------------------------------------------------------------------------- //
// Snapshot JSON parsing (the hook's POST /snapshot body)
// --------------------------------------------------------------------------- //

static void apply_snapshot_json(const cJSON *root)
{
    claudi_snapshot_t tmp;
    claudi_snapshot_init(&tmp);

    const cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "total")) && cJSON_IsNumber(j)) {
        tmp.total = (int32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "running")) && cJSON_IsNumber(j)) {
        tmp.running = (int32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "waiting"))) {
        // hook sends 0/1 (number) or bool
        tmp.waiting = cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valuedouble != 0);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "msg")) && cJSON_IsString(j)) {
        strncpy(tmp.msg, j->valuestring, CLAUDI_MSG_LEN - 1);
    }

    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (cJSON_IsArray(entries)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, entries) {
            if (cJSON_IsString(e)) {
                claudi_snapshot_add_entry(&tmp, e->valuestring);
            }
        }
    }

    const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(root, "prompt");
    if (cJSON_IsObject(prompt)) {
        tmp.prompt.set = true;
        if ((j = cJSON_GetObjectItemCaseSensitive(prompt, "id")) && cJSON_IsString(j)) {
            strncpy(tmp.prompt.id, j->valuestring, CLAUDI_ID_LEN - 1);
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(prompt, "tool")) && cJSON_IsString(j)) {
            strncpy(tmp.prompt.tool, j->valuestring, CLAUDI_TOOL_LEN - 1);
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(prompt, "hint")) && cJSON_IsString(j)) {
            strncpy(tmp.prompt.hint, j->valuestring, CLAUDI_HINT_LEN - 1);
        }
    }

    tmp.overlay = CLAUDI_STATE_COUNT;  // none
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "overlay")) && cJSON_IsString(j)) {
        claudi_state_t ov;
        if (claudi_state_from_name(j->valuestring, &ov)) {
            tmp.overlay = ov;
            tmp.overlay_ms = 2500;  // default; overridden below
        }
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "overlay_ms")) && cJSON_IsNumber(j)) {
        tmp.overlay_ms = (uint32_t)j->valuedouble;
    }

    tmp.updated_ms = now_ms();

    lock();
    s_snap = tmp;
    unlock();

    ESP_LOGI(TAG, "snapshot: running=%d waiting=%d total=%d overlay=%s msg='%s'",
             (int)tmp.running, (int)tmp.waiting, (int)tmp.total,
             tmp.overlay == CLAUDI_STATE_COUNT ? "-" : claudi_state_name(tmp.overlay),
             tmp.msg);
}

// --------------------------------------------------------------------------- //
// HTTP handlers
// --------------------------------------------------------------------------- //

static esp_err_t snapshot_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > CLAUDI_SNAPSHOT_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body length");
        return ESP_FAIL;
    }
    char *buf = malloc(total + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    apply_snapshot_json(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    claudi_snapshot_t snap;
    claudi_net_get_snapshot(&snap);
    claudi_derived_t d = claudi_derive(&snap, now_ms());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "claudi");
    cJSON_AddStringToObject(root, "effective", claudi_state_name(d.effective));
    cJSON_AddStringToObject(root, "base", claudi_state_name(d.base));
    cJSON_AddStringToObject(root, "overlay",
                            d.overlay_active ? claudi_state_name(d.overlay) : "-");
    cJSON_AddBoolToObject(root, "stale", d.stale);
    cJSON_AddNumberToObject(root, "running", snap.running);
    cJSON_AddNumberToObject(root, "total", snap.total);
    cJSON_AddBoolToObject(root, "waiting", snap.waiting);
    cJSON_AddStringToObject(root, "msg", snap.msg);
    cJSON_AddStringToObject(root, "ip", s_ip);
    cJSON_AddNumberToObject(root, "uptime_ms", now_ms());
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return ESP_OK;
}

// Legacy fallback: GET /pet/state?state=working — force a state for ~1h. Our
// firmware implements /snapshot, so the hook never falls back here; this exists
// only for manual `curl` testing and old hosts.
static esp_err_t pet_state_get_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char value[24] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "state", value, sizeof(value)) == ESP_OK) {
        claudi_state_t st;
        if (claudi_state_from_name(value, &st)) {
            lock();
            claudi_snapshot_init(&s_snap);
            s_snap.overlay = st;
            s_snap.overlay_ms = 3600000u;  // ~1h
            s_snap.updated_ms = now_ms();
            unlock();
            httpd_resp_sendstr(req, "ok");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown state");
    return ESP_FAIL;
}

static esp_err_t render_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "legacy /render (ignored in V1)");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/snapshot",  .method = HTTP_POST, .handler = snapshot_post_handler},
        {.uri = "/status",    .method = HTTP_GET,  .handler = status_get_handler},
        {.uri = "/pet/state", .method = HTTP_GET,  .handler = pet_state_get_handler},
        {.uri = "/render",    .method = HTTP_GET,  .handler = render_get_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server up: POST /snapshot, GET /status, /pet/state, /render");
}

// --------------------------------------------------------------------------- //
// Wi-Fi + mDNS
// --------------------------------------------------------------------------- //

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected, ip=%s, http://%s.local/", s_ip, CLAUDI_MDNS_HOST);
        if (s_httpd == NULL) {
            start_httpd();
        }
    }
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed");
        return;
    }
    mdns_hostname_set(CLAUDI_MDNS_HOST);
    mdns_instance_name_set("claudi companion");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void claudi_net_start(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }
    lock();
    claudi_snapshot_init(&s_snap);
    s_snap.updated_ms = now_ms();
    unlock();

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CLAUDI_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CLAUDI_WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns();
    ESP_LOGI(TAG, "claudi_net started, joining SSID '%s'", CLAUDI_WIFI_SSID);
}
