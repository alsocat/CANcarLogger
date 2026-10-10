// Trip logger: polls the engine ECU over CAN (can.cpp), integrates distance
// and estimated fuel, and writes trips to flash.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "esp_flash.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "shared.h"

static const char *TAG = "logger";

uint32_t g_boot_id = 0;

// Free flash after the two 4MB OTA slots. Listed in partitions.csv for future
// USB flashes; registered at runtime on boards flashed before it existed.
#define LOGS_OFFSET 0x820000
#define LOGS_SIZE 0x7E0000

#define END_AFTER_MS 15000     // engine off / ECU silent this long ends a trip
#define RECORD_EVERY_MS 500
#define FLUSH_EVERY_MS 2000  // what a power cut can lose
// Pedal this far (raw, /255) above its resting position = hard acceleration:
// record every loop (~10 Hz) and skip the slow groups so 0-60 etc. can be timed.
#define BURST_PEDAL 90
// Full health scan (all modules, ~15 s) only every this many starts or km,
// or when the engine's MIL / DTC count changes, or on request.
#define SCAN_EVERY_STARTS 20
#define SCAN_EVERY_KM 480  // ~300 mi

std::atomic<bool> scan_requested{false};
// Health scan progress for the dashboard: requests done / total, 0 total = not scanning
static std::atomic<int> scan_step{0}, scan_steps{0};
std::atomic<int> clear_requested{0};
char clear_target[16], clear_dtc[8];
static char clear_buf[96] = "";  // outcome of the last clear request, for the live view
static char health_buf[96] = "no check yet";

const char *health_note()
{
    return health_buf;
}

// Latest decoded values (raw ECU bytes, see Record)
static Record live = {};
static int64_t live_updated_us = 0;
static float live_fuel_rate = 0;  // mL/s per litre displacement, or mL/s with MAF
static float live_maf = 0;        // g/s (PID 10), cars that report it
static SemaphoreHandle_t live_lock;

static enum { ST_NO_CAN, ST_ENGINE_OFF, ST_TRIP } state = ST_NO_CAN;

float logger_ecu_volts()
{
    return esp_timer_get_time() - live_updated_us < 10000000 ? live.voltage_mv / 1000.0f : 0;
}

bool logger_idle()
{
    return state != ST_TRIP;
}

const char *logger_state()
{
    switch (state) {
    case ST_NO_CAN: return "CAN bus not ready";
    case ST_ENGINE_OFF: return "engine off";
    case ST_TRIP: return "recording";
    }
    return "?";
}

// ---------- storage ----------

static bool storage_init()
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logs");
    if (!p) {
        esp_err_t err = esp_partition_register_external(esp_flash_default_chip, LOGS_OFFSET, LOGS_SIZE, "logs",
                                                        ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &p);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "can't register logs partition: %s", esp_err_to_name(err));
            return false;
        }
    }
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/logs";
    conf.partition_label = "logs";
    conf.format_if_mount_failed = true;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("logs", &total, &used);
    ESP_LOGI(TAG, "log storage %u KB used of %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return true;
}

static std::vector<std::string> list_trips(const char *ext)
{
    std::vector<std::string> names;
    DIR *dir = opendir("/logs");
    if (!dir) {
        return names;
    }
    while (dirent *e = readdir(dir)) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcmp(dot, ext) == 0) {
            names.emplace_back(e->d_name);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// Trips cut short by power loss are finished here.
static void finalize_leftovers()
{
    for (auto &name : list_trips(".tmp")) {
        std::string tmp = "/logs/" + name;
        if (name[0] == 'h') {  // half-written health scan
            unlink(tmp.c_str());
            continue;
        }
        struct stat st;
        if (stat(tmp.c_str(), &st) == 0 && st.st_size > (off_t)sizeof(TripHeader)) {
            std::string bin = tmp.substr(0, tmp.size() - 4) + ".bin";
            rename(tmp.c_str(), bin.c_str());
            ESP_LOGI(TAG, "finalized %s", bin.c_str());
        } else {
            unlink(tmp.c_str());
        }
    }
}

static void make_room()
{
    size_t total = 0, used = 0;
    // Keep room for the next trip: 512 KB, or 1/8 of a small (SuperMini) partition.
    while (esp_littlefs_info("logs", &total, &used) == ESP_OK && total - used < std::min<size_t>(512 * 1024, total / 8)) {
        auto trips = list_trips(".bin");
        if (trips.empty()) {
            break;
        }
        ESP_LOGW(TAG, "storage full, dropping oldest trip %s", trips.front().c_str());
        unlink(("/logs/" + trips.front()).c_str());
    }
}

static uint32_t nvs_get(const char *key, uint32_t dflt)
{
    nvs_handle_t h;
    uint32_t v = dflt;
    if (nvs_open("logger", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_put(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("logger", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint32_t nvs_bump(const char *key)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("logger", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        v++;
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
    return v;
}

// ---------- ECU requests ----------

// Sends a request written as hex ("010C0D") to a module and returns the reply
// bytes: 0 = no answer, -1 = couldn't send.
static int ecu(const char *hex, uint8_t *out, int max, int timeout_ms = 300, uint16_t tx = 0x7E0,
               uint16_t rx = 0x7E8)
{
    uint8_t req[7];
    int n = 0;
    for (const char *p = hex; p[0] && p[1] && n < 7; p += 2) {
        char b[3] = {p[0], p[1], 0};
        req[n++] = strtol(b, nullptr, 16);
    }
    return can_request(tx, rx, req, n, out, max, timeout_ms);
}

static int pid_len(uint8_t pid)
{
    switch (pid) {
    case 0x03: case 0x0C: case 0x10: case 0x1F: case 0x23: case 0x31: case 0x3C:
    case 0x42: case 0x43: case 0x44:
        return 2;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0B: case 0x0D: case 0x0E:
    case 0x0F: case 0x11: case 0x2F: case 0x33: case 0x46: case 0x49:
        return 1;
    default:
        return -1;
    }
}

// Returns number of PIDs decoded into `live`.
static int apply(const uint8_t *b, int n)
{
    if (n < 3 || b[0] != 0x41) {
        return 0;
    }
    int count = 0;
    xSemaphoreTake(live_lock, portMAX_DELAY);
    for (int i = 1; i < n;) {
        uint8_t pid = b[i++];
        int len = pid_len(pid);
        if (len < 0 || i + len > n) {
            break;
        }
        const uint8_t *d = b + i;
        uint16_t w = len == 2 ? (d[0] << 8) | d[1] : d[0];
        switch (pid) {
        case 0x03: live.fuel_status = d[0]; break;
        case 0x04: live.load = w; break;
        case 0x05: live.coolant = w; break;
        case 0x06: live.stft = w; break;
        case 0x07: live.ltft = w; break;
        case 0x0B: live.map_kpa = w; break;
        case 0x0C: live.rpm = w / 4; break;
        case 0x0D: live.speed_kph = w; break;
        case 0x0E: live.timing = w; break;
        case 0x0F: live.iat = w; break;
        case 0x11: live.throttle = w; break;
        case 0x23: live.rail = w; break;
        case 0x2F: live.fuel_level = w; break;
        case 0x33: live.baro_kpa = w; break;
        case 0x3C: live.cat_temp = w; break;
        case 0x42: live.voltage_mv = w; break;
        case 0x43: live.abs_load = w; break;
        case 0x44: live.lambda = w; break;
        case 0x46: live.ambient = w; break;
        case 0x49: live.pedal = w; break;
        case 0x10: live_maf = w / 100.0f; break;
        }
        i += len;
        count++;
    }
    live_updated_us = esp_timer_get_time();
    xSemaphoreGive(live_lock);
    return count;
}

static std::atomic<int64_t> last_answer_us{0};

int64_t logger_last_answer_us()
{
    return last_answer_us.load();
}

static int query(const char *cmd)
{
    if (!cmd[0]) {
        return 0;
    }
    static uint8_t bytes[96];
    int n = ecu(cmd, bytes, sizeof(bytes));
    int got = n > 0 ? apply(bytes, n) : 0;
    if (got) {
        last_answer_us.store(esp_timer_get_time());
    }
    return got;
}

// ---------- which car this is ----------
// The VIN (mode 09 PID 02) picks VW extras (UDS odometer, VW module list);
// the supported-PID bitmaps (01 00/20/40) decide what gets polled, so the same
// firmware runs on cars without a MAF PID (fuel from load, e.g. VW MQB) and with one.

static char vin[18];
static bool is_vw = true;      // until the VIN says otherwise
static uint32_t pid_mask[3];   // PIDs 01-20, 21-40, 41-60 from 0100/0120/0140
static bool pids_known = false;
static bool identified = false;
static char grp_fast[16], grp_a[16], grp_b[16], grp_c[16];

const char *vehicle_vin()
{
    return vin;
}

static bool supported(uint8_t pid)
{
    if (!pids_known) {
        return pid != 0x10;  // the VW MQB set: everything we ask for except MAF
    }
    int bank = (pid - 1) / 32;
    return bank < 3 && (pid_mask[bank] >> (31 - (pid - 1) % 32)) & 1;
}

// "01" + up to 6 of the given PIDs this car supports, in order.
static void make_group(char *out, const uint8_t *pids, int n)
{
    int k = 0, used = 0;
    k += sprintf(out, "01");
    for (int i = 0; i < n && used < 6; i++) {
        if (supported(pids[i])) {
            k += sprintf(out + k, "%02X", pids[i]);
            used++;
        }
    }
    if (!used) {
        out[0] = 0;
    }
}

static void build_groups()
{
    // Fast: rpm, speed, MAF, lambda, fuel status, pedal, abs load (first 6 the car has)
    static const uint8_t fast[] = {0x0C, 0x0D, 0x10, 0x44, 0x03, 0x49, 0x43};
    static const uint8_t a[] = {0x0B, 0x0F, 0x05, 0x06, 0x07, 0x04};
    static const uint8_t b[] = {0x0E, 0x11, 0x33, 0x2F, 0x42, 0x46};
    static const uint8_t c[] = {0x3C, 0x23, 0x43};
    make_group(grp_fast, fast, sizeof(fast));
    make_group(grp_a, a, sizeof(a));
    make_group(grp_b, b, sizeof(b));
    make_group(grp_c, c, sizeof(c));
    ESP_LOGI(TAG, "polling %s | %s | %s | %s", grp_fast, grp_a, grp_b, grp_c);
}

static void identify_car()
{
    uint8_t b[64];
    for (int bank = 0; bank < 3; bank++) {
        static const char *req[] = {"0100", "0120", "0140"};
        int n = ecu(req[bank], b, sizeof(b), 500);
        if (n >= 6 && b[0] == 0x41) {
            pid_mask[bank] = (b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5];
            pids_known = true;
        }
        if (!(pid_mask[bank] & 1)) {
            break;  // no next bank
        }
    }
    int n = ecu("0902", b, sizeof(b), 1000);
    if (n >= 20 && b[0] == 0x49 && b[1] == 0x02) {
        memcpy(vin, b + n - 17, 17);
        vin[17] = 0;
        for (char &c : vin) {
            if (c && (c < '0' || c > 'Z')) c = '?';
        }
        static const char *VW_WMI[] = {"WVW", "WVG", "WV1", "WV2", "WAU", "WA1", "WUA", "TRU", "3VW", "1VW", "9BW"};
        is_vw = false;
        for (const char *w : VW_WMI) {
            is_vw |= strncmp(vin, w, 3) == 0;
        }
    }
    if (!vin[0]) {
        // No VIN reply: call it a VW only if the VW odometer DID answers.
        int m = ecu("22295A", b, sizeof(b), 500);
        is_vw = m >= 3 && b[0] == 0x62;
    }
    if (pids_known) {
        build_groups();
    } else {  // car didn't list its PIDs: the VW MQB set
        strcpy(grp_fast, "010C0D43440349");
        strcpy(grp_a, "010B0F05060704");
        strcpy(grp_b, "010E11332F4246");
        strcpy(grp_c, "013C23");
    }
    identified = true;
    ESP_LOGI(TAG, "car %s (%s), PIDs %08lx %08lx %08lx", vin[0] ? vin : "VIN unknown", is_vw ? "VW" : "generic",
             (unsigned long)pid_mask[0], (unsigned long)pid_mask[1], (unsigned long)pid_mask[2]);
}

// VW-specific: the engine ECU keeps a copy of the odometer (km) at DID 295A.
static void query_odo()
{
    if (!is_vw) {
        return;
    }
    uint8_t b[16];
    int n = ecu("22295A", b, sizeof(b), 1000);
    if (n >= 6 && b[0] == 0x62 && b[1] == 0x29 && b[2] == 0x5A) {
        xSemaphoreTake(live_lock, portMAX_DELAY);
        live.odo_km = (b[3] << 16) | (b[4] << 8) | b[5];
        xSemaphoreGive(live_lock);
    }
}

// ---------- health scan ----------
// On every ignition-on: UDS ReadDTCInformation (19 02, pending|confirmed|
// failed) from each module, plus OBD mode 01 PID 01 (MIL) and mode 06
// misfire counters from the engine. Raw replies are uploaded as hex and
// decoded on the server.

struct Module {
    const char *name;
    uint16_t tx, rx;
};
static const Module MODULES[] = {
    {"engine", 0x7E0, 0x7E8},   {"abs", 0x713, 0x77D},     {"airbag", 0x715, 0x77F},
    {"gateway", 0x710, 0x77A},  {"cluster", 0x714, 0x77E}, {"central", 0x70E, 0x778},
    {"hvac", 0x746, 0x7B0},     {"steering", 0x70C, 0x776}, {"epas", 0x712, 0x77C},
};

static char uds_raw[16];  // why a module gave nothing, for the server

static int uds(const char *req, uint8_t *out, int max, int timeout_ms = 2000, uint16_t tx = 0x7E0,
               uint16_t rx = 0x7E8)
{
    int n = ecu(req, out, max, timeout_ms, tx, rx);
    snprintf(uds_raw, sizeof(uds_raw), "%s", n < 0 ? "send failed" : n == 0 ? "no reply" : "");
    return n;
}

static void append_hex(std::string &s, const uint8_t *b, int n)
{
    static const char *H = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        s += H[b[i] >> 4];
        s += H[b[i] & 15];
    }
}

static void health_scan(const std::string &extra = "")
{
    static uint8_t b[640];
    std::string js = "{\"modules\":[";
    int64_t t0 = esp_timer_get_time();
    scan_step = 0;
    scan_steps = (is_vw ? sizeof(MODULES) / sizeof(MODULES[0]) + 6 : 2 * 3 + 8);
    bool first = true;
    // Modules often answer DTC reads with "response pending" first;
    // can_request keeps waiting for the real answer.
    // Other makes: OBD stored (03), pending (07) and permanent (0A) codes from
    // the engine and transmission ECUs instead of VW's module list.
    static const Module GENERIC[] = {{"engine", 0x7E0, 0x7E8}, {"trans", 0x7E1, 0x7E9}};
    if (!is_vw) {
        for (const Module &m : GENERIC) {
            js += first ? "{" : ",{";
            first = false;
            js += "\"name\":\"";
            js += m.name;
            js += "\"";
            static const char *modes[] = {"03", "07", "0A"};
            for (const char *mode : modes) {
                int n = uds(mode, b, sizeof(b), 2000, m.tx, m.rx);
                scan_step++;
                js += ",\"obd";
                js += mode;
                js += "\":\"";
                append_hex(js, b, n > 0 ? n : 0);
                js += "\"";
            }
            js += "}";
        }
    }
    for (const Module &m : MODULES) {
        if (!is_vw) {
            break;
        }
        int n = uds("19020D", b, sizeof(b), 3000, m.tx, m.rx);
        scan_step++;
        js += first ? "{" : ",{";
        first = false;
        js += "\"name\":\"";
        js += m.name;
        js += "\",\"resp\":\"";
        append_hex(js, b, n > 0 ? n : 0);
        if (n <= 0) {
            js += "\",\"raw\":\"";
            js += uds_raw;
        }
        js += "\"}";
    }
    js += "],\"pid01\":\"";
    int n = uds("0101", b, sizeof(b));
    scan_step++;
    append_hex(js, b, n > 0 ? n : 0);
    js += "\",\"mode06\":[";
    // Misfire monitors: A1 = all cylinders, A2.. = cylinder 1.. (up to 6)
    const char *mids[] = {"06A1", "06A2", "06A3", "06A4", "06A5", "06A6", "06A7"};
    for (int i = 0; i < (is_vw ? 5 : 7); i++) {
        n = uds(mids[i], b, sizeof(b));
        scan_step++;
        js += i ? ",\"" : "\"";
        append_hex(js, b, n > 0 ? n : 0);
        js += "\"";
    }
    char tail[80];
    snprintf(tail, sizeof(tail), "],\"odo_km\":%lu,\"vin\":\"%s\"}", (unsigned long)live.odo_km, vin);
    js += tail;
    if (!extra.empty()) {  // "...}" -> "...,<extra>}"
        js.pop_back();
        js += ",";
        js += extra;
        js += "}";
    }

    make_room();
    TripHeader h = {};
    h.magic = HEALTH_MAGIC;
    h.version = 1;
    h.trip_id = nvs_bump("scan_ctr");
    h.boot_id = g_boot_id;
    h.start_uptime_ms = esp_timer_get_time() / 1000;
    time_t now = time(nullptr);
    h.start_epoch = now > 1700000000 ? now : 0;
    char tmp[32], path[32];
    snprintf(tmp, sizeof(tmp), "/logs/h%06lu.tmp", (unsigned long)h.trip_id);
    snprintf(path, sizeof(path), "/logs/h%06lu.hlt", (unsigned long)h.trip_id);
    if (FILE *f = fopen(tmp, "wb")) {
        fwrite(&h, sizeof(h), 1, f);
        fwrite(js.data(), 1, js.size(), f);
        fclose(f);
        rename(tmp, path);
        pending_trips.fetch_add(1);
    }
    scan_steps = 0;
    ESP_LOGI(TAG, "health scan: %u bytes in %lld ms", (unsigned)js.size(),
             (long long)(esp_timer_get_time() - t0) / 1000);
}

// ---------- clearing fault codes ----------
// Only on request from the dashboard, with the ignition on and the engine
// off. Engine (and transmission on other makes): OBD mode 04. "All" on a VW
// also sends UDS ClearDiagnosticInformation (14 FF FF FF) to each module, in
// the extended session. Returns a JSON fragment with what each one said.

static std::string clear_codes(int scope)
{
    static uint8_t b[64];
    static const char *names[] = {"", "engine", "all", "module", "code"};
    std::string js = "\"clear\":{\"scope\":\"";
    js += names[scope];
    js += "\",\"target\":\"";
    js += scope >= CLEAR_MODULE ? clear_target : "";
    js += "\",\"dtc\":\"";
    js += scope == CLEAR_CODE ? clear_dtc : "";
    js += "\",\"results\":[";
    bool first = true;
    auto result = [&](const char *name, int n, uint8_t ok_sid) {
        js += first ? "{" : ",{";
        first = false;
        js += "\"name\":\"";
        js += name;
        js += "\",\"ok\":";
        js += n > 0 && b[0] == ok_sid ? "true" : "false";
        js += ",\"resp\":\"";
        append_hex(js, b, n > 0 ? (n > 8 ? 8 : n) : 0);
        js += "\"}";
    };
    // OBD mode 04 clears the engine (all of its codes; it can't pick one)
    auto clear_obd = [&](const char *name, uint16_t tx, uint16_t rx, bool must_answer) {
        int n = ecu("04", b, sizeof(b), 3000, tx, rx);
        if (n > 0 || must_answer) {
            result(name, n, 0x44);
        }
    };
    // UDS ClearDiagnosticInformation for a VW module: FFFFFF = all, or one DTC
    auto clear_uds = [&](const Module &m, const char *dtc) {
        char req[16];
        snprintf(req, sizeof(req), "14%s", dtc);
        ecu("1003", b, sizeof(b), 1000, m.tx, m.rx);
        int n = ecu(req, b, sizeof(b), 5000, m.tx, m.rx);
        result(m.name, n, 0x54);
    };
    bool dtc_ok = strlen(clear_dtc) == 6 && strspn(clear_dtc, "0123456789ABCDEFabcdef") == 6;

    if (scope == CLEAR_ENGINE || scope == CLEAR_ALL ||
        (scope == CLEAR_MODULE && strcmp(clear_target, "engine") == 0)) {
        clear_obd("engine", 0x7E0, 0x7E8, true);
    }
    if (!is_vw && (scope == CLEAR_ENGINE || scope == CLEAR_ALL ||
                   (scope == CLEAR_MODULE && strcmp(clear_target, "trans") == 0))) {
        clear_obd("trans", 0x7E1, 0x7E9, scope == CLEAR_MODULE);
    }
    if (is_vw) {
        for (const Module &m : MODULES) {
            if (m.tx == 0x7E0) {
                continue;  // engine: mode 04 above
            }
            if (scope == CLEAR_ALL || (scope == CLEAR_MODULE && strcmp(clear_target, m.name) == 0)) {
                clear_uds(m, "FFFFFF");
            } else if (scope == CLEAR_CODE && dtc_ok && strcmp(clear_target, m.name) == 0) {
                clear_uds(m, clear_dtc);
            }
        }
    }
    js += "]}";
    ESP_LOGW(TAG, "cleared codes: %s", js.c_str());
    return js;
}

static void handle_clear_request(bool engine_running)
{
    int scope = clear_requested.exchange(0);
    if (!scope) {
        return;
    }
    unsigned up = esp_timer_get_time() / 1000000;
    if (engine_running) {
        snprintf(clear_buf, sizeof(clear_buf), "refused at %us: engine running", up);
        return;
    }
    snprintf(clear_buf, sizeof(clear_buf), "clearing at %us", up);
    std::string res = clear_codes(scope);
    snprintf(clear_buf, sizeof(clear_buf), "cleared at %us, scanning", up);
    snprintf(health_buf, sizeof(health_buf), "full scan at %us (after clearing codes)", up);
    vTaskDelay(pdMS_TO_TICKS(2000));  // let modules settle before reading them back
    health_scan(res);
    snprintf(clear_buf, sizeof(clear_buf), "cleared at %us", up);
}

// Every ignition-on: a quick look at the engine's MIL / DTC count (one
// request). The full scan only runs when that changed, when it's been a
// while, or when the dashboard asked for one.
static void maybe_health_scan(bool forced)
{
    uint8_t b[16];
    uint32_t mil = 0xFFFFFFFF;  // unknown
    {
        int n = ecu("0101", b, sizeof(b));
        if (n >= 3 && b[0] == 0x41 && b[1] == 0x01) {
            mil = b[2];  // bit 7 = MIL on, low bits = DTC count
        }
    }
    uint32_t last_mil = nvs_get("scan_mil", 0xFFFFFFFF);
    uint32_t last_boot = nvs_get("scan_boot", 0);
    uint32_t last_odo = nvs_get("scan_odo", 0);
    const char *why = forced ? "requested"
                      : last_boot == 0 ? "first scan"
                      : (mil != 0xFFFFFFFF && mil != last_mil) ? "engine fault status changed"
                      : g_boot_id - last_boot >= SCAN_EVERY_STARTS ? "starts since last scan"
                      : (live.odo_km > last_odo && live.odo_km - last_odo >= SCAN_EVERY_KM) ? "distance since last scan"
                      : nullptr;
    unsigned up = esp_timer_get_time() / 1000000;
    if (!why) {
        snprintf(health_buf, sizeof(health_buf), "quick check at %us: mil byte %02lx, full scan not due (%lu starts since)",
                 up, (unsigned long)mil, (unsigned long)(g_boot_id - last_boot));
        return;
    }
    snprintf(health_buf, sizeof(health_buf), "full scan at %us (%s)", up, why);
    health_scan();
    if (mil != 0xFFFFFFFF) {
        nvs_put("scan_mil", mil);
    }
    nvs_put("scan_boot", g_boot_id);
    if (live.odo_km) {
        nvs_put("scan_odo", live.odo_km);
    }
}

// ---------- trips ----------

static FILE *trip_file = nullptr;
static char trip_path[32];
static uint32_t trip_id;
// Guards trip_file/trip_path against the uploader reading the trip in progress.
static SemaphoreHandle_t trip_lock;
static int64_t trip_start_us;
static std::vector<Record> pending;
static float dist_m, fuel_ml;
static uint8_t pedal_min;

static void flush_trip()
{
    if (!trip_file || pending.empty()) {
        return;
    }
    xSemaphoreTake(trip_lock, portMAX_DELAY);
    fwrite(pending.data(), sizeof(Record), pending.size(), trip_file);
    fflush(trip_file);
    fsync(fileno(trip_file));
    xSemaphoreGive(trip_lock);
    pending.clear();
}

bool logger_trip_info(uint32_t *id, int64_t *start_us, long *size)
{
    if (!trip_lock) {
        return false;
    }
    xSemaphoreTake(trip_lock, portMAX_DELAY);
    bool ok = trip_file != nullptr;
    if (ok) {
        *id = trip_id;
        *start_us = trip_start_us;
        fseek(trip_file, 0, SEEK_END);
        *size = ftell(trip_file);
    }
    xSemaphoreGive(trip_lock);
    return ok;
}

long logger_read_trip(uint32_t id, long offset, void *buf, size_t max)
{
    xSemaphoreTake(trip_lock, portMAX_DELAY);
    long n = -1;
    if (trip_file && trip_id == id && fseek(trip_file, offset, SEEK_SET) == 0) {
        n = fread(buf, 1, max, trip_file);
        fseek(trip_file, 0, SEEK_END);  // the logger appends
    }
    xSemaphoreGive(trip_lock);
    return n;
}

static void start_trip()
{
    make_room();
    TripHeader h = {};
    h.magic = TRIP_MAGIC;
    h.version = TRIP_VERSION;
    h.trip_id = nvs_bump("trip_ctr");
    h.boot_id = g_boot_id;
    trip_start_us = esp_timer_get_time();
    h.start_uptime_ms = trip_start_us / 1000;
    time_t now = time(nullptr);
    h.start_epoch = now > 1700000000 ? now : 0;
    h.record_size = sizeof(Record);
    h.reserved[0] = supported(0x10) ? TRIP_FLAG_FUEL_ML : 0;

    xSemaphoreTake(trip_lock, portMAX_DELAY);
    snprintf(trip_path, sizeof(trip_path), "/logs/%06lu.tmp", (unsigned long)h.trip_id);
    trip_id = h.trip_id;
    trip_file = fopen(trip_path, "w+b");
    if (trip_file) {
        fwrite(&h, sizeof(h), 1, trip_file);
        fflush(trip_file);
    }
    xSemaphoreGive(trip_lock);
    if (!trip_file) {
        ESP_LOGE(TAG, "can't create %s", trip_path);
        return;
    }
    dist_m = fuel_ml = 0;
    pedal_min = 255;
    pending.clear();
    state = ST_TRIP;
    ESP_LOGI(TAG, "trip %lu started", (unsigned long)h.trip_id);
}

static void end_trip()
{
    if (trip_file) {
        flush_trip();
        // Started before the clock was set but it's set now: fill in the real
        // start time while we still know how long ago that was.
        xSemaphoreTake(trip_lock, portMAX_DELAY);
        TripHeader h;
        time_t now = time(nullptr);
        if (fseek(trip_file, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, trip_file) == 1 &&
            h.start_epoch == 0 && now > 1700000000) {
            h.start_epoch = now - (esp_timer_get_time() - trip_start_us) / 1000000;
            fseek(trip_file, 0, SEEK_SET);
            fwrite(&h, sizeof(h), 1, trip_file);
            fflush(trip_file);
            fsync(fileno(trip_file));
        }
        fclose(trip_file);
        trip_file = nullptr;
        std::string bin(trip_path);
        bin.replace(bin.size() - 4, 4, ".bin");
        rename(trip_path, bin.c_str());
        xSemaphoreGive(trip_lock);
        pending_trips.fetch_add(1);
        ESP_LOGI(TAG, "trip finished: %.1f km, %s", dist_m / 1000, bin.c_str());
    }
    state = ST_ENGINE_OFF;
}

bool logger_live_json(char *out, size_t len)
{
    // Values go stale during a health scan, but keep reporting its progress
    if (!live_lock || (esp_timer_get_time() - live_updated_us > 3000000 && !scan_steps)) {
        return false;
    }
    xSemaphoreTake(live_lock, portMAX_DELAY);
    Record r = live;
    float rate = live_fuel_rate;
    xSemaphoreGive(live_lock);
    snprintf(out, len,
             "{\"state\":\"%s\",\"rpm\":%u,\"speed_kph\":%u,\"coolant_c\":%d,\"iat_c\":%d,"
             "\"map_kpa\":%u,\"baro_kpa\":%u,\"throttle\":%.1f,\"pedal\":%.1f,\"load\":%.1f,"
             "\"abs_load\":%.1f,\"lambda\":%.3f,\"stft\":%.1f,\"ltft\":%.1f,\"timing\":%.1f,"
             "\"fuel_level\":%.1f,\"ambient_c\":%d,\"voltage\":%.2f,\"cat_c\":%.0f,"
             "\"fuel_status\":%u,\"fuel_rate_ml_s_per_l\":%.4f,\"trip_dist_m\":%.0f,"
             "\"trip_fuel_ml_per_l\":%.2f,\"odo_km\":%lu,\"maf\":%.2f,\"fuel_abs\":%d,\"vin\":\"%s\",\"clear_note\":\"%s\","
             "\"scan\":\"%s\",\"scan_step\":%d,\"scan_steps\":%d}",
             logger_state(), r.rpm, r.speed_kph, r.coolant - 40, r.iat - 40, r.map_kpa, r.baro_kpa,
             r.throttle * 100 / 255.0, r.pedal * 100 / 255.0, r.load * 100 / 255.0,
             r.abs_load * 100 / 255.0, r.lambda * 2 / 65536.0, (r.stft - 128) * 100 / 128.0,
             (r.ltft - 128) * 100 / 128.0, r.timing / 2.0 - 64, r.fuel_level * 100 / 255.0, r.ambient - 40,
             r.voltage_mv / 1000.0, r.cat_temp / 10.0 - 40, r.fuel_status, rate,
             state == ST_TRIP ? dist_m : 0, state == ST_TRIP ? fuel_ml : 0, (unsigned long)r.odo_km, live_maf,
             supported(0x10) ? 1 : 0, vin, clear_buf,
             scan_steps ? "running" : scan_requested ? "queued" : "", scan_step.load(), scan_steps.load());
    return true;
}

// Distance from speed, fuel from the air going in:
//   with MAF (PID 10): air g/s straight from the sensor -> fuel in mL
//   without (VW MQB):  air g/s per litre = abs_load% / 100 * 1.184 g/L * rpm / 120
//                      -> fuel in mL per litre of displacement (server scales)
//   fuel mL/s = air / (14.7 * commanded lambda) / 0.745 g/mL
// Zero during decel fuel cut (open loop, foot off pedal, engine above idle).
static void integrate(float dt, uint16_t prev_speed)
{
    xSemaphoreTake(live_lock, portMAX_DELAY);
    Record r = live;
    xSemaphoreGive(live_lock);

    dist_m += (prev_speed + r.speed_kph) / 2.0f / 3.6f * dt;
    uint8_t accel = supported(0x49) ? r.pedal : r.throttle;
    if (accel < pedal_min) {
        pedal_min = accel;
    }
    float lambda = r.lambda * 2 / 65536.0f;
    if (lambda < 0.6f || lambda > 1.6f) {
        lambda = 1.0f;
    }
    bool fuel_cut = r.fuel_status == 4 && accel <= pedal_min + 3 && r.rpm > 1000;
    float rate = 0;
    if (r.rpm > 0 && !fuel_cut) {
        float air = supported(0x10) ? live_maf : r.abs_load * 100 / 255.0f / 100 * 1.184f * r.rpm / 120;
        rate = air / (14.7f * lambda) / 0.745f;
    }
    fuel_ml += rate * dt;
    live_fuel_rate = rate;
}

static void logger_task(void *)
{
    int64_t last_good_us = 0, rpm_zero_since = 0, last_record_us = 0, last_flush_us = 0, last_fast_us = 0;
    int64_t last_awake_us = 0;
    bool scanned = false;  // health scan done for this ignition cycle
    uint32_t loop = 0;

    while (true) {
        if (!can_ready()) {
            // A bus-off mid-drive recovers in ~10 s; only give up on the trip
            // if it lasts longer than an engine-off would.
            if (state == ST_TRIP && esp_timer_get_time() - last_good_us < END_AFTER_MS * 1000LL) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (state == ST_TRIP) {
                end_trip();
            }
            state = ST_NO_CAN;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (state == ST_NO_CAN) {
            state = ST_ENGINE_OFF;
        }

        int64_t now = esp_timer_get_time();
        if (state != ST_TRIP) {
            // Engine off: check slowly so a sleeping ECU isn't kept awake
            bool awake = query("010C0D") > 0;
            if (awake && !identified) {
                identify_car();
            }
            if (awake) {
                last_awake_us = esp_timer_get_time();
                handle_clear_request(live.rpm > 0);
                bool forced = scan_requested.exchange(false);
                if (!scanned || forced) {
                    query_odo();
                    maybe_health_scan(forced);
                    scanned = true;
                }
                // Ignition on, engine off: keep the vitals current too
                // (battery voltage, coolant, fuel level, odometer).
                query(grp_a);
                query(grp_b);
                query_odo();
            }
            if (awake && live.rpm > 0) {
                start_trip();
                last_good_us = rpm_zero_since = last_fast_us = esp_timer_get_time();
                last_record_us = last_flush_us = 0;
                loop = 0;
            } else {
                int64_t asleep_us = esp_timer_get_time() - last_awake_us;
                if (!awake && asleep_us > 60 * 1000000LL) {
                    scanned = false;  // ignition has been off a while
                }
                // Each request can wake the car's gateway, so once the car has
                // been silent a while ask less often (boards without working
                // sleep end up here for the whole time the car is parked).
                vTaskDelay(pdMS_TO_TICKS(awake ? 2000 : asleep_us > 600 * 1000000LL ? 60000
                                                      : asleep_us > 180 * 1000000LL ? 15000 : 2000));
            }
            continue;
        }

        if (clear_requested.load()) {
            handle_clear_request(true);  // refused: never while the engine runs
        }
        // "Scan now" with the engine running: only while stopped, since the
        // scan pauses logging for a few seconds.
        if (live.speed_kph == 0 && scan_requested.exchange(false)) {
            maybe_health_scan(true);
            last_good_us = rpm_zero_since = last_fast_us = esp_timer_get_time();
        }
        // Fast group every loop: rpm, speed, abs load, lambda, fuel status, pedal
        uint16_t prev_speed = live.speed_kph;
        if (query(grp_fast) > 0) {
            now = esp_timer_get_time();
            float dt = (now - last_fast_us) / 1e6f;
            if (dt < 2.0f) {
                integrate(dt, prev_speed);
            }
            last_fast_us = now;
            last_good_us = now;
            if (live.rpm > 0) {
                rpm_zero_since = now;
            }
        }
        bool burst = (supported(0x49) ? live.pedal : live.throttle) > pedal_min + BURST_PEDAL;
        // Slower groups interleaved (paused during hard acceleration)
        if (burst) {
        } else if (loop % 4 == 1) {
            query(grp_a);  // MAP, IAT, coolant, trims, load
        } else if (loop % 4 == 3) {
            query(grp_b);  // timing, throttle, baro, fuel level, volts, ambient
        } else if (loop % 40 == 2) {
            query(grp_c);  // cat temp, rail pressure (abs load if it didn't fit above)
        } else if (loop % 40 == 22) {
            query_odo();
        }
        loop++;

        now = esp_timer_get_time();
        if (burst || now - last_record_us >= RECORD_EVERY_MS * 1000) {
            xSemaphoreTake(live_lock, portMAX_DELAY);
            Record r = live;
            xSemaphoreGive(live_lock);
            r.t_ms = (now - trip_start_us) / 1000;
            r.dist_m = dist_m;
            r.fuel_ml_per_l = fuel_ml;
            pending.push_back(r);
            last_record_us = now;
        }
        if (now - last_flush_us >= FLUSH_EVERY_MS * 1000) {
            flush_trip();
            last_flush_us = now;
        }
        if (now - rpm_zero_since > END_AFTER_MS * 1000 || now - last_good_us > END_AFTER_MS * 1000) {
            end_trip();
        }
    }
}

void logger_start()
{
    live_lock = xSemaphoreCreateMutex();
    trip_lock = xSemaphoreCreateMutex();
    g_boot_id = nvs_bump("boot_ctr");
    if (storage_init()) {
        finalize_leftovers();
        xTaskCreate(logger_task, "logger", 6144, nullptr, 4, nullptr);
    }
}
