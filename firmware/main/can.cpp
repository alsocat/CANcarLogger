// Direct CAN to the car's diagnostic bus (OBD pins 6/14) through an
// SN65HVD230 transceiver on the S3's TWAI controller, 500 kbit/s 11-bit.
// Requests are ISO-TP (ISO 15765-2): a single frame out, single or
// multi-frame reply back, with our flow control sent to the ECU's request id.
//
// On VW the OBD port sits behind the gateway and is usually silent until a
// tester asks something, so "no traffic" in listen-only mode proves nothing.

#include <atomic>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "hal/adc_types.h"
#include "nvs.h"

#include "shared.h"

static const char *TAG = "can";

#define CAN_TX_GPIO GPIO_NUM_4  // to transceiver CTX
#define CAN_RX_GPIO GPIO_NUM_5  // from transceiver CRX
#define CAN_BITRATE 500000
#define CAN_PAD 0xAA            // filler for unused bytes; VW ECUs want 8-byte frames
#define BUS_OFF_RETRY_MS 10000  // stay off the bus this long after a bus-off

struct RxFrame {
    uint32_t id;
    uint8_t len;
    uint8_t d[8];
};

static twai_node_handle_t node;
static QueueHandle_t rxq;
static SemaphoreHandle_t req_lock;
static std::atomic<bool> listen_only{false};
static std::atomic<int> err_state{TWAI_ERROR_ACTIVE};
static std::atomic<uint32_t> rx_frames{0}, tx_ok{0}, tx_fail{0}, bus_errors{0}, bus_offs{0};
static std::atomic<int64_t> bus_off_at{0};

// Distinct ids seen, for the status page (mostly useful in listen-only mode)
#define SEEN_MAX 48
static uint16_t seen_id[SEEN_MAX];
static uint32_t seen_n[SEEN_MAX];
static std::atomic<int> seen_count{0};

static bool IRAM_ATTR on_rx(twai_node_handle_t h, const twai_rx_done_event_data_t *, void *)
{
    uint8_t buf[8];
    twai_frame_t f = {};
    f.buffer = buf;
    f.buffer_len = sizeof(buf);
    if (twai_node_receive_from_isr(h, &f) != ESP_OK || f.header.ide || f.header.rtr) {
        return false;
    }
    rx_frames++;
    int n = seen_count.load();
    int i = 0;
    while (i < n && seen_id[i] != f.header.id) {
        i++;
    }
    if (i < n) {
        seen_n[i]++;
    } else if (n < SEEN_MAX) {
        seen_id[n] = f.header.id;
        seen_n[n] = 1;
        seen_count.store(n + 1);
    }
    RxFrame r;
    r.id = f.header.id;
    r.len = f.header.dlc > 8 ? 8 : f.header.dlc;
    memcpy(r.d, buf, r.len);
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rxq, &r, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t, const twai_tx_done_event_data_t *e, void *)
{
    if (e->is_tx_success) {
        tx_ok++;
    } else {
        tx_fail++;
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t, const twai_error_event_data_t *, void *)
{
    bus_errors++;
    return false;
}

static bool IRAM_ATTR on_state(twai_node_handle_t, const twai_state_change_event_data_t *e, void *)
{
    err_state.store(e->new_sta);
    if (e->new_sta == TWAI_ERROR_BUS_OFF) {
        bus_offs++;
        bus_off_at.store(esp_timer_get_time());
    }
    return false;
}

static bool node_create(bool listen)
{
    twai_onchip_node_config_t cfg = {};
    cfg.io_cfg.tx = CAN_TX_GPIO;
    cfg.io_cfg.rx = CAN_RX_GPIO;
    cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    cfg.bit_timing.bitrate = CAN_BITRATE;
    cfg.fail_retry_cnt = 3;  // don't keep hammering a bus that isn't answering
    cfg.tx_queue_depth = 8;
    cfg.flags.enable_listen_only = listen;
    if (twai_new_node_onchip(&cfg, &node) != ESP_OK) {
        ESP_LOGE(TAG, "can't create TWAI node");
        node = nullptr;
        return false;
    }
    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = on_rx;
    cbs.on_tx_done = on_tx_done;
    cbs.on_error = on_error;
    cbs.on_state_change = on_state;
    twai_node_register_event_callbacks(node, &cbs, nullptr);
    err_state.store(TWAI_ERROR_ACTIVE);
    if (twai_node_enable(node) != ESP_OK) {
        twai_node_delete(node);
        node = nullptr;
        return false;
    }
    listen_only.store(listen);
    ESP_LOGI(TAG, "CAN up at %d kbit/s, %s", CAN_BITRATE / 1000, listen ? "listen-only" : "normal");
    return true;
}

// Brings the node back after a bus-off, but only after a pause so a wiring
// fault can't flood the car's bus with error frames.
static void can_watch_task(void *)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (node && err_state.load() == TWAI_ERROR_BUS_OFF &&
            esp_timer_get_time() - bus_off_at.load() > BUS_OFF_RETRY_MS * 1000LL) {
            ESP_LOGW(TAG, "recovering from bus-off");
            bus_off_at.store(esp_timer_get_time());
            twai_node_recover(node);
        }
    }
}

void can_start()
{
    if (rxq) {  // already up (the early restart check started it)
        return;
    }
    rxq = xQueueCreate(64, sizeof(RxFrame));
    req_lock = xSemaphoreCreateMutex();
    uint8_t listen = 0;
    nvs_handle_t h;
    if (nvs_open("can", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "listen", &listen);
        nvs_close(h);
    }
    node_create(listen);
    xTaskCreate(can_watch_task, "can_watch", 2560, nullptr, 3, nullptr);
}

bool can_set_listen_only(bool listen)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = nullptr;
    }
    bool ok = node_create(listen);
    xSemaphoreGive(req_lock);
    nvs_handle_t h;
    if (nvs_open("can", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "listen", listen);
        nvs_commit(h);
        nvs_close(h);
    }
    return ok;
}

void can_stop()
{
    if (!req_lock) {
        return;
    }
    xSemaphoreTake(req_lock, portMAX_DELAY);
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = nullptr;
    }
    xSemaphoreGive(req_lock);
}

bool can_ready()
{
    return node && !listen_only.load() && err_state.load() != TWAI_ERROR_BUS_OFF;
}

static bool send_frame(uint16_t id, const uint8_t *d, int n)
{
    uint8_t buf[8];
    memset(buf, CAN_PAD, sizeof(buf));
    memcpy(buf, d, n);
    twai_frame_t f = {};
    f.header.id = id;
    f.header.dlc = 8;
    f.buffer = buf;
    f.buffer_len = 8;
    if (twai_node_transmit(node, &f, 50) != ESP_OK) {
        return false;
    }
    // buf lives on our stack, so wait for the controller to finish with it
    return twai_node_transmit_wait_all_done(node, 100) == ESP_OK;
}

int can_request(uint16_t tx, uint16_t rx, const uint8_t *req, int len, uint8_t *out, int max, int timeout_ms)
{
    if (len < 1 || len > 7 || !can_ready()) {
        return -1;
    }
    xSemaphoreTake(req_lock, portMAX_DELAY);
    xQueueReset(rxq);
    uint8_t sf[8];
    sf[0] = len;
    memcpy(sf + 1, req, len);
    if (!send_frame(tx, sf, len + 1)) {
        xSemaphoreGive(req_lock);
        return -1;
    }
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    int got = 0, total = -1;
    uint8_t next_seq = 1;
    int result = 0;  // 0 = no (complete) answer in time
    while (true) {
        int64_t left = (deadline - esp_timer_get_time()) / 1000;
        RxFrame f;
        if (left <= 0 || xQueueReceive(rxq, &f, pdMS_TO_TICKS(left)) != pdTRUE) {
            break;
        }
        if (f.id != rx || f.len < 1) {
            continue;
        }
        uint8_t pci = f.d[0] >> 4;
        if (pci == 0 && total < 0) {  // single frame
            int n = f.d[0] & 0x0F;
            if (n >= 3 && f.d[1] == 0x7F && f.d[3] == 0x78) {
                deadline = esp_timer_get_time() + 5000 * 1000LL;  // "response pending"
                continue;
            }
            n = n > f.len - 1 ? f.len - 1 : n;
            n = n > max ? max : n;
            memcpy(out, f.d + 1, n);
            result = n;
            break;
        }
        if (pci == 1 && total < 0) {  // first frame
            total = ((f.d[0] & 0x0F) << 8) | f.d[1];
            for (int i = 2; i < f.len && got < total; i++) {
                if (got < max) {
                    out[got] = f.d[i];
                }
                got++;
            }
            const uint8_t fc[3] = {0x30, 0x00, 0x00};  // continue, no block limit, no gap
            if (!send_frame(tx, fc, 3)) {
                break;
            }
            deadline = esp_timer_get_time() + 1000 * 1000LL;
            continue;
        }
        if (pci == 2 && total >= 0) {  // consecutive frame
            if ((f.d[0] & 0x0F) != (next_seq & 0x0F)) {
                ESP_LOGW(TAG, "%03lx: frame out of sequence", (unsigned long)rx);
                break;
            }
            next_seq++;
            for (int i = 1; i < f.len && got < total; i++) {
                if (got < max) {
                    out[got] = f.d[i];
                }
                got++;
            }
            if (got >= total) {
                result = total > max ? max : total;
                break;
            }
            deadline = esp_timer_get_time() + 1000 * 1000LL;
        }
    }
    xSemaphoreGive(req_lock);
    return result;
}

void can_status(char *out, size_t len)
{
    static const char *states[] = {"ok", "warning", "error-passive", "bus-off"};
    if (!node) {
        snprintf(out, len, "not started");
        return;
    }
    twai_node_status_t st = {};
    twai_node_get_info(node, &st, nullptr);
    int n = snprintf(out, len, "%s, %s, tx %lu ok / %lu failed, rx %lu frames, bus errors %lu, bus-offs %lu (tec %u rec %u)",
                     listen_only.load() ? "listen-only" : "normal", states[err_state.load() & 3],
                     (unsigned long)tx_ok.load(), (unsigned long)tx_fail.load(), (unsigned long)rx_frames.load(),
                     (unsigned long)bus_errors.load(), (unsigned long)bus_offs.load(), st.tx_error_count,
                     st.rx_error_count);
    int count = seen_count.load();
    if (count && n > 0 && (size_t)n < len) {
        n += snprintf(out + n, len - n, "\nids seen:");
        for (int i = 0; i < count && n > 0 && (size_t)n < len; i++) {
            n += snprintf(out + n, len - n, " %03X(%lu)", seen_id[i], (unsigned long)seen_n[i]);
        }
    }
}

// Wiring check with the CAN controller off. Each pin is read with the ESP's
// weak pull-up and then pull-down: a pin the transceiver drives (its R/CRX
// output) keeps its level, a pin that only goes into the transceiver (its
// D/CTX input) or to nothing follows the pull. Then, if that looks right, TX
// is driven dominant/recessive and RX should follow (the transceiver hears
// itself). Only use this off the car: it bypasses the controller's bus rules.
static int level_with_pull(gpio_num_t pin, bool up)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, up ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(2000);
    int high = 0;
    for (int i = 0; i < 200; i++) {
        high += gpio_get_level(pin);
        esp_rom_delay_us(50);
    }
    return high;  // out of 200
}

static const char *pin_kind(int up, int down)
{
    if (up > 190 && down > 190) return "driven high";
    if (up < 10 && down < 10) return "driven low";
    if (up > 190 && down < 10) return "follows the pull (input or unconnected)";
    return "noisy";
}

void can_probe(char *out, size_t len)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    bool listen = listen_only.load();
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = nullptr;
    }
    int tx_up = level_with_pull(CAN_TX_GPIO, true), tx_dn = level_with_pull(CAN_TX_GPIO, false);
    int rx_up = level_with_pull(CAN_RX_GPIO, true), rx_dn = level_with_pull(CAN_RX_GPIO, false);
    int n = snprintf(out, len,
                     "GPIO%d (should go to CTX): %s [%d/%d high]\n"
                     "GPIO%d (should come from CRX): %s [%d/%d high]\n",
                     CAN_TX_GPIO, pin_kind(tx_up, tx_dn), tx_up, tx_dn, CAN_RX_GPIO, pin_kind(rx_up, rx_dn), rx_up,
                     rx_dn);
    // Drive one pin dominant/recessive and see whether the other follows.
    // A wrong guess only fights the transceiver's output for microseconds.
    auto loop = [](gpio_num_t drive, gpio_num_t sense, int *dom, int *rec) {
        gpio_reset_pin(sense);
        gpio_set_direction(sense, GPIO_MODE_INPUT);
        gpio_reset_pin(drive);
        gpio_set_direction(drive, GPIO_MODE_OUTPUT);
        *dom = *rec = 0;
        for (int i = 0; i < 20; i++) {
            gpio_set_level(drive, 0);
            esp_rom_delay_us(20);
            *dom += gpio_get_level(sense) == 0;
            gpio_set_level(drive, 1);
            esp_rom_delay_us(20);
            *rec += gpio_get_level(sense) == 1;
        }
        gpio_set_level(drive, 1);
        gpio_reset_pin(drive);
    };
    int dom, rec;
    loop(CAN_TX_GPIO, CAN_RX_GPIO, &dom, &rec);
    n += snprintf(out + n, len - n, "drive GPIO%d, read GPIO%d: followed %d/20 dominant, %d/20 recessive\n",
                  CAN_TX_GPIO, CAN_RX_GPIO, dom, rec);
    if (dom == 20 && rec == 20) {
        snprintf(out + n, len - n, "-> ESP <-> transceiver OK (CTX/CRX right, transceiver powered, driver works)\n");
    } else {
        int sdom, srec;
        loop(CAN_RX_GPIO, CAN_TX_GPIO, &sdom, &srec);
        n += snprintf(out + n, len - n, "drive GPIO%d, read GPIO%d: followed %d/20 dominant, %d/20 recessive\n",
                      CAN_RX_GPIO, CAN_TX_GPIO, sdom, srec);
        snprintf(out + n, len - n, "%s",
                 sdom == 20 && srec == 20
                     ? "-> CTX and CRX are SWAPPED (GPIO4 is on CRX, GPIO5 on CTX)\n"
                     : "-> the transceiver doesn't echo either way: check its 3V3/GND, and that CANH and CANL aren't "
                       "shorted together\n");
    }
    node_create(listen);
    xSemaphoreGive(req_lock);
}

// Holds TX dominant (low) for `seconds` with the controller off, so the
// CTX pin and CANH/CANL can be measured with a meter. Desk use only.
void can_hold_dominant(int seconds)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    bool listen = listen_only.load();
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = nullptr;
    }
    gpio_reset_pin(CAN_TX_GPIO);
    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_TX_GPIO, 0);
    ESP_LOGW(TAG, "holding TX dominant for %d s", seconds);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    gpio_set_level(CAN_TX_GPIO, 1);
    gpio_reset_pin(CAN_TX_GPIO);
    node_create(listen);
    xSemaphoreGive(req_lock);
}

// Bench analysis with CANH on GPIO6 and CANL on GPIO7 (ADC1 ch5/ch6): drives
// CTX and samples the bus pins and CRX over time, so a driver that never
// switches, or one that switches and then times out, shows up. Desk only.
static int mv(adc_channel_t ch)
{
    return adc1_mv(ch);  // power.cpp owns ADC1
}

void can_analyze(char *out, size_t len)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    bool listen = listen_only.load();
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = nullptr;
    }
    gpio_reset_pin(CAN_RX_GPIO);
    gpio_set_direction(CAN_RX_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CAN_RX_GPIO, GPIO_FLOATING);
    gpio_reset_pin(CAN_TX_GPIO);
    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_OUTPUT);

    int n = snprintf(out, len, "%-26s %6s %6s %6s %4s\n", "", "CANH", "CANL", "H-L", "CRX");
    auto row = [&](const char *label) {
        int h = mv(ADC_CHANNEL_5), l = mv(ADC_CHANNEL_6), r = gpio_get_level(CAN_RX_GPIO);
        if (n > 0 && (size_t)n < len) {
            n += snprintf(out + n, len - n, "%-26s %6d %6d %6d %4d\n", label, h, l, h - l, r);
        }
    };
    gpio_set_level(CAN_TX_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    row("recessive (CTX high)");
    gpio_set_level(CAN_TX_GPIO, 0);
    row("dominant, at once");
    esp_rom_delay_us(200);
    row("dominant, 0.2 ms");
    esp_rom_delay_us(800);
    row("dominant, 1 ms");
    vTaskDelay(pdMS_TO_TICKS(9));
    row("dominant, 10 ms");
    vTaskDelay(pdMS_TO_TICKS(90));
    row("dominant, 100 ms");
    vTaskDelay(pdMS_TO_TICKS(900));
    row("dominant, 1 s");
    gpio_set_level(CAN_TX_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    row("recessive again");
    // A 50%-duty square wave at roughly CAN bit rate, sampled mid-dominant:
    // what the transceiver sees when it's actually transmitting.
    int dom_low = 0, dom_seen = 0;
    for (int i = 0; i < 200; i++) {
        gpio_set_level(CAN_TX_GPIO, 0);
        esp_rom_delay_us(2);
        dom_low += gpio_get_level(CAN_RX_GPIO) == 0;
        dom_seen += mv(ADC_CHANNEL_5) - mv(ADC_CHANNEL_6) > 900;
        gpio_set_level(CAN_TX_GPIO, 1);
        esp_rom_delay_us(20);
    }
    if (n > 0 && (size_t)n < len) {
        snprintf(out + n, len - n, "short pulses: CRX low %d/200, H-L > 0.9 V %d/200 (ADC is slow; CRX is the real test)\n",
                 dom_low, dom_seen);
    }
    gpio_reset_pin(CAN_TX_GPIO);
    node_create(listen);
    xSemaphoreGive(req_lock);
}
