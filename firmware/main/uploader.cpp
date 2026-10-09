// Ships finished trips (and a live snapshot twice a second) to the carlog
// server whenever the home WiFi is reachable, and streams the trip being
// recorded so a drive shows up as soon as the car is back in range, even if
// the power is cut before the trip ends. A trip file is deleted only after
// the server confirms it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"

#include "shared.h"

static const char *TAG = "upload";
extern uint32_t g_boot_id;

static char server_ip[16];
static int64_t resolved_at_us = 0;

static bool resolve_server()
{
    const char *host = CONFIG_OBD_SERVER_HOST;
    size_t n = strlen(host);
    if (n < 6 || strcmp(host + n - 6, ".local") != 0) {
        strncpy(server_ip, host, sizeof(server_ip) - 1);
        return true;
    }
    if (server_ip[0] && esp_timer_get_time() - resolved_at_us < 600 * 1000000LL) {
        return true;
    }
    esp_ip4_addr_t addr = {};
    if (mdns_query_a(std::string(host, n - 6).c_str(), 2000, &addr) != ESP_OK) {
        return server_ip[0] != 0;  // keep using the last known address
    }
    snprintf(server_ip, sizeof(server_ip), IPSTR, IP2STR(&addr));
    resolved_at_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s is %s", host, server_ip);
    return true;
}

static esp_http_client_handle_t make_client(const char *path, int timeout_ms)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", server_ip, CONFIG_OBD_SERVER_PORT, path);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "X-Token", CONFIG_OBD_SERVER_TOKEN);
    static char device[13];
    if (!device[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(device, sizeof(device), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    esp_http_client_set_header(c, "X-Device", device);  // this board, in case the VIN can't be read
    if (vehicle_vin()[0]) {
        esp_http_client_set_header(c, "X-Car", vehicle_vin());  // which car, for the server
    }
    return c;
}

// The server answers 202 when the dashboard wants something from the car:
// {"scan": true} for "Scan now", {"clear": "engine"|"all"} for "Clear codes".
static void post_live()
{
    static char json[1024];
    if (!logger_live_json(json, sizeof(json))) {
        return;
    }
    esp_http_client_handle_t c = make_client("/api/live", 2000);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int len = strlen(json);
    if (esp_http_client_open(c, len) != ESP_OK || esp_http_client_write(c, json, len) != len) {
        resolved_at_us = 0;  // re-resolve next time in case the server moved
        esp_http_client_cleanup(c);
        return;
    }
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) == 202) {
        char resp[160] = {};
        esp_http_client_read(c, resp, sizeof(resp) - 1);
        if (strstr(resp, "\"scan\":true")) {
            scan_requested.store(true);
        }
        if (strstr(resp, "\"clear\":\"engine\"")) {
            clear_requested.store(CLEAR_ENGINE);
        } else if (strstr(resp, "\"clear\":\"all\"")) {
            clear_requested.store(CLEAR_ALL);
        }
    }
    esp_http_client_cleanup(c);
}

static bool upload_trip(const std::string &name)
{
    std::string path = "/logs/" + name;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    struct stat st;
    fstat(fileno(f), &st);
    TripHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1 || (h.magic != TRIP_MAGIC && h.magic != HEALTH_MAGIC)) {
        fclose(f);
        unlink(path.c_str());  // corrupt, nothing to recover
        return true;
    }
    fseek(f, 0, SEEK_SET);

    // Trips recorded before the clock was set get their start time now, as
    // long as the device hasn't rebooted since (uptime is still comparable).
    int64_t start_epoch = h.start_epoch;
    time_t now = time(nullptr);
    if (!start_epoch && h.boot_id == g_boot_id && now > 1700000000) {
        start_epoch = now - (esp_timer_get_time() / 1000 - h.start_uptime_ms) / 1000;
    }

    esp_http_client_handle_t c = make_client(h.magic == HEALTH_MAGIC ? "/api/health" : "/api/upload", 20000);
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    char epoch[24];
    snprintf(epoch, sizeof(epoch), "%lld", (long long)start_epoch);
    esp_http_client_set_header(c, "X-Start-Epoch", epoch);

    bool ok = false;
    if (esp_http_client_open(c, st.st_size) == ESP_OK) {
        static char buf[2048];
        size_t n;
        bool sent = true;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (esp_http_client_write(c, buf, n) != (int)n) {
                sent = false;
                break;
            }
        }
        if (sent) {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            ok = status == 200 || status == 409;  // 409 = already have it
            ESP_LOGI(TAG, "%s (%ld bytes) -> HTTP %d", name.c_str(), (long)st.st_size, status);
        }
    }
    esp_http_client_cleanup(c);
    fclose(f);
    if (ok) {
        unlink(path.c_str());
    }
    return ok;
}

// Sends the part of the current trip the server doesn't have yet. The server
// appends it to the trip (marked in progress) and answers with how many bytes
// it now holds; if that disagrees with our count it says where to resume.
static uint32_t synced_trip;
static long synced_bytes;

static void sync_current_trip()
{
    uint32_t id;
    int64_t start_us;
    long size;
    if (!logger_trip_info(&id, &start_us, &size)) {
        return;
    }
    if (id != synced_trip) {
        synced_trip = id;
        synced_bytes = 0;
    }
    static char buf[16384];
    for (int tries = 0; synced_bytes < size && tries < 32; tries++) {
        long n = logger_read_trip(id, synced_bytes, buf, sizeof(buf));
        if (n <= 0) {
            return;  // trip just ended; it gets uploaded as a finished file
        }
        uploading.store(true);
        esp_http_client_handle_t c = make_client("/api/upload", 20000);
        esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(c, "X-Partial", "1");
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)id);
        esp_http_client_set_header(c, "X-Trip-Id", hdr);
        snprintf(hdr, sizeof(hdr), "%ld", synced_bytes);
        esp_http_client_set_header(c, "X-Offset", hdr);
        time_t now = time(nullptr);
        if (now > 1700000000) {
            snprintf(hdr, sizeof(hdr), "%lld",
                     (long long)(now - (esp_timer_get_time() - start_us) / 1000000));
            esp_http_client_set_header(c, "X-Start-Epoch", hdr);
        }
        int status = -1;
        long have = -1;
        if (esp_http_client_open(c, n) == ESP_OK && esp_http_client_write(c, buf, n) == n) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            char resp[128] = {};
            esp_http_client_read(c, resp, sizeof(resp) - 1);
            if (const char *p = strstr(resp, "\"have\":")) {
                have = atol(p + 7);
            }
        }
        esp_http_client_cleanup(c);
        uploading.store(false);
        if (status == 200 || status == 416) {
            synced_bytes = have >= 0 ? have : (status == 200 ? synced_bytes + n : 0);
            ESP_LOGI(TAG, "trip %lu in progress: server has %ld of %ld bytes", (unsigned long)id,
                     synced_bytes, size);
        } else if (status == 409) {
            synced_bytes = size;  // server already has the finished trip
        } else {
            return;  // unreachable; next round
        }
    }
}

static void uploader_task(void *)
{
    int64_t last_scan_us = 0;
    bool was_up = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));  // live view at 2 Hz
        if (!wifi_up.load() || !resolve_server()) {
            was_up = false;
            continue;
        }
        post_live();

        // Right after (re)joining WiFi, e.g. pulling into the driveway, and
        // then every 10 s: finished files first, then the trip in progress.
        bool just_joined = !was_up;
        was_up = true;
        if (!just_joined && esp_timer_get_time() - last_scan_us < 10 * 1000000LL) {
            continue;
        }
        last_scan_us = esp_timer_get_time();
        std::vector<std::string> names;
        if (DIR *dir = opendir("/logs")) {
            while (dirent *e = readdir(dir)) {
                const char *dot = strrchr(e->d_name, '.');
                if (dot && (strcmp(dot, ".bin") == 0 || strcmp(dot, ".hlt") == 0)) {
                    names.emplace_back(e->d_name);
                }
            }
            closedir(dir);
        }
        std::sort(names.begin(), names.end());
        pending_trips.store(names.size());
        bool reachable = true;
        if (!names.empty()) {
            uploading.store(true);
            for (auto &name : names) {
                if (!upload_trip(name)) {
                    reachable = false;  // server unreachable; try again later
                    break;
                }
                pending_trips.fetch_sub(1);
            }
            uploading.store(false);
        }
        if (reachable) {
            sync_current_trip();
        }
    }
}

void uploader_start()
{
    xTaskCreate(uploader_task, "uploader", 6144, nullptr, 3, nullptr);
}
