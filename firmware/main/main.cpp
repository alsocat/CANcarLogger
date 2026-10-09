// ESP32-S3 car logger on the OBD port's diagnostic CAN bus.
//
// SN65HVD230 transceiver -> ESP32-S3: 3V3 -> 3V3, GND -> GND, CTX -> GPIO4,
// CRX -> GPIO5; CANH -> OBD pin 6, CANL -> OBD pin 14.
//
// Trips are logged to flash and uploaded to the carlog server (logger.cpp,
// uploader.cpp). http://obd-bridge.local/ shows status; /uds sends one
// read-only diagnostic request (tools/vwprobe.py).
//
// Firmware updates over WiFi: ./ota.sh (POSTs the build to http://obd-bridge.local/update).
// A new image that crashes or can't get onto WiFi in its first minutes is rolled back.

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/ip4_addr.h"

#include "shared.h"

static const char *TAG = "obd";

std::atomic<bool> wifi_up{false};
static std::atomic<bool> pending_verify{false};

// ---------- OTA ----------

static void mark_app_good()
{
    if (pending_verify.exchange(false)) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

// A new image is accepted once it has stayed up 45 s with WiFi connected; any
// crash or reboot before that (or 2 minutes without WiFi) rolls back.
bool ota_pending_verify()
{
    return pending_verify.load();
}

static void rollback_watchdog(void *)
{
    for (int s = 0; s < 120; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s >= 45 && wifi_up.load()) {
            mark_app_good();
            vTaskDelete(nullptr);
        }
    }
    ESP_LOGE(TAG, "new firmware never got online, rolling back");
    esp_restart();
}

static esp_err_t info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    static char can[512], pwr[256], body[1280];
    power_status(pwr, sizeof(pwr));
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    can_status(can, sizeof(can));
    wifi_ap_record_t ap = {};
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    snprintf(body, sizeof(body), "obd-wifi-bridge\nbuilt %s %s (%s)\npartition %s\nwifi %d dBm, channel %d\npower %s\ncan %s\nlogger %s\nhealth %s%s\n",
             app->date, app->time, sha, esp_ota_get_running_partition()->label, rssi, ap.primary, pwr, can,
             logger_state(), health_note(), scan_requested.load() ? " (scan requested)" : "");
    return httpd_resp_sendstr(req, body);
}

static bool token_ok(httpd_req_t *req)
{
    char token[65] = {};
    return strlen(CONFIG_OBD_OTA_TOKEN) == 0 ||
           (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token)) == ESP_OK &&
            strcmp(token, CONFIG_OBD_OTA_TOKEN) == 0);
}

// POST /can?listen=1|0 switches listen-only mode (remembered across boots);
// POST /can?probe=1 runs the desk wiring check (never on the car).
static esp_err_t can_handler(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
    }
    char q[32] = {}, v[4] = {};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "probe", v, sizeof(v)) == ESP_OK) {
        static char report[512];
        can_probe(report, sizeof(report));
        return httpd_resp_sendstr(req, report);
    }
    if (httpd_query_key_value(q, "analyze", v, sizeof(v)) == ESP_OK) {
        static char report[1024];
        can_analyze(report, sizeof(report));
        return httpd_resp_sendstr(req, report);
    }
    if (httpd_query_key_value(q, "hold", v, sizeof(v)) == ESP_OK) {
        int secs = atoi(v) > 0 && atoi(v) <= 120 ? atoi(v) : 30;
        httpd_resp_sendstr(req, "holding TX dominant\n");
        can_hold_dominant(secs);
        return ESP_OK;
    }
    if (httpd_query_key_value(q, "listen", v, sizeof(v)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?listen=1, ?listen=0 or ?probe=1");
    }
    can_set_listen_only(v[0] == '1');
    return info_handler(req);
}

// POST /uds?tx=7E0&rx=7E8 with a hex request body ("22295A") -> hex reply.
// Read-only services only; coding/writes are deliberately not possible here.
static esp_err_t uds_handler(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
    }
    char q[48] = {}, v[8] = {}, body[20] = {};
    uint16_t tx = 0x7E0, rx = 0x7E8;
    int timeout = 2000;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "tx", v, sizeof(v)) == ESP_OK) tx = strtol(v, nullptr, 16);
        if (httpd_query_key_value(q, "rx", v, sizeof(v)) == ESP_OK) rx = strtol(v, nullptr, 16);
        if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) timeout = atoi(v);
    }
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    uint8_t r[8];
    int len = 0;
    for (int i = 0; i + 1 < n && len < 7; i += 2) {
        char b[3] = {body[i], body[i + 1], 0};
        r[len++] = strtol(b, nullptr, 16);
    }
    static const uint8_t allowed[] = {0x01, 0x06, 0x09, 0x10, 0x19, 0x22, 0x3E};
    if (!len || !memchr(allowed, r[0], sizeof(allowed)) || (r[0] == 0x10 && len > 1 && r[1] == 0x02)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "only read-only services 01 06 09 10 19 22 3E");
    }
    static uint8_t out[1024];
    static char hex[2100];
    int got = can_request(tx, rx, r, len, out, sizeof(out), timeout > 15000 ? 15000 : timeout);
    if (got < 0) {
        return httpd_resp_sendstr(req, "SEND FAILED\n");
    }
    if (got == 0) {
        return httpd_resp_sendstr(req, "NO REPLY\n");
    }
    for (int i = 0; i < got; i++) {
        sprintf(hex + i * 2, "%02X", out[i]);
    }
    strcat(hex, "\n");
    return httpd_resp_sendstr(req, hex);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    char token[65] = {};
    if (strlen(CONFIG_OBD_OTA_TOKEN) > 0 &&
        (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token)) != ESP_OK ||
         strcmp(token, CONFIG_OBD_OTA_TOKEN) != 0)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t ota;
    power_hold.store(true);  // no deep sleep halfway through an update
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota) != ESP_OK) {
        power_hold.store(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }
    ESP_LOGI(TAG, "receiving %d byte update into %s", req->content_len, part->label);

    static char buf[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || esp_ota_write(ota, buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            power_hold.store(false);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        remaining -= n;
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        power_hold.store(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
    }

    httpd_resp_sendstr(req, "OK, rebooting\n");
    ESP_LOGI(TAG, "update written, rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void http_start()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.recv_wait_timeout = 30;
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t info = {};
    info.uri = "/";
    info.method = HTTP_GET;
    info.handler = info_handler;
    httpd_register_uri_handler(server, &info);
    httpd_uri_t update = {};
    update.uri = "/update";
    update.method = HTTP_POST;
    update.handler = update_handler;
    httpd_register_uri_handler(server, &update);
    httpd_uri_t can = {};
    can.uri = "/can";
    can.method = HTTP_POST;
    can.handler = can_handler;
    httpd_register_uri_handler(server, &can);
    httpd_uri_t uds = {};
    uds.uri = "/uds";
    uds.method = HTTP_POST;
    uds.handler = uds_handler;
    httpd_register_uri_handler(server, &uds);
}

// ---------- WiFi side ----------

static void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_up.store(false);
        auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "WiFi lost (reason %d, rssi %d), reconnecting", ev->reason, ev->rssi);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        wifi_up.store(true);
        ESP_LOGI(TAG, "connected as obd-bridge.local / " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static void wifi_start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));

    wifi_config_t cfg = {};
    if (strlen(CONFIG_OBD_STA_SSID) > 0) {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, "obd-bridge");
        strncpy((char *)cfg.sta.ssid, CONFIG_OBD_STA_SSID, sizeof(cfg.sta.ssid));
        strncpy((char *)cfg.sta.password, CONFIG_OBD_STA_PASS, sizeof(cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_LOGI(TAG, "joining WiFi \"%s\"", CONFIG_OBD_STA_SSID);
        ESP_ERROR_CHECK(mdns_init());
        mdns_hostname_set("obd-bridge");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp);
    } else {
        esp_netif_t *ap = esp_netif_create_default_wifi_ap();
        esp_netif_ip_info_t ip = {};
        IP4_ADDR(&ip.ip, 192, 168, 0, 10);
        IP4_ADDR(&ip.gw, 192, 168, 0, 10);
        IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap);
        esp_netif_set_ip_info(ap, &ip);
        esp_netif_dhcps_start(ap);

        strncpy((char *)cfg.ap.ssid, CONFIG_OBD_AP_SSID, sizeof(cfg.ap.ssid));
        strncpy((char *)cfg.ap.password, CONFIG_OBD_AP_PASS, sizeof(cfg.ap.password));
        cfg.ap.ssid_len = strlen(CONFIG_OBD_AP_SSID);
        cfg.ap.max_connection = 2;
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
        mark_app_good();
        ESP_LOGI(TAG, "AP \"%s\" up at 192.168.0.10", CONFIG_OBD_AP_SSID);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // lower latency
}

extern "C" void app_main(void)
{
    power_early_check();  // a timer wake with the engine off ends here
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        pending_verify.store(true);
        xTaskCreate(rollback_watchdog, "rollback", 2048, nullptr, 1, nullptr);
    }

    led_start();
    wifi_start();
    http_start();
    can_start();
    logger_start();
    uploader_start();
    power_start();
}
