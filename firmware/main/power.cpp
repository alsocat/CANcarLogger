// Deep sleep while the car is off, for a board left on the OBD port's
// always-on pin 16.
//
// Wakes on:
//   - a timer every OBD_SLEEP_CHECK_S: a ~0.3 s boot reads the battery and goes
//     straight back to sleep unless the alternator is charging (engine running)
//   - CAN activity (CRX going dominant), on cars whose OBD port carries traffic.
//     If that keeps waking us without an ECU ever answering (noise, or a car
//     that chats while parked) it's switched off for a while.
//
// Battery sense: car 12 V -- 1 MOhm --+-- 220 kOhm -- GND, junction -> the
// OBD_VBAT_GPIO pin (+100 nF to GND). Without it only CAN activity can wake
// the board, which isn't enough on cars whose OBD port is silent (VW).

#include <atomic>
#include <cmath>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"

#include "shared.h"

static const char *TAG = "power";

#define CAN_TX_GPIO GPIO_NUM_4
#define CAN_RX_GPIO GPIO_NUM_5
#define WAKE_VOLTS (CONFIG_OBD_WAKE_VOLTS_X10 / 10.0f)
#define MIN_AWAKE_S 90         // after any wake, so a short stop doesn't bounce
#define ECU_SILENT_S 120       // car counts as off this long after its last answer
#define UPLOAD_GRACE_S 600     // longest we stay up past that to finish uploads
#define NOISE_WAKES_MAX 3      // CAN wakes in a row with no ECU answer...
#define NOISE_BACKOFF_SLEEPS 90  // ...turn CAN wake off for this many sleeps (~30 min)

RTC_DATA_ATTR static uint32_t rtc_sleeps, rtc_checks, rtc_can_wakes, rtc_noise_streak, rtc_can_off_until;
RTC_DATA_ATTR static char rtc_last_reason[48];
// The divider reading has matched the ECU's own battery voltage (PID 42).
// Until then we never sleep: an empty or floating pin could read anything,
// and a board that can't tell the engine started would miss whole trips.
RTC_DATA_ATTR static bool rtc_vsense_ok;

std::atomic<bool> power_hold{false};  // set while an OTA update is running
static char why_awake[64] = "starting";
static int64_t awake_since_us;
static bool woke_by_can;

// ---------- ADC (shared with can.cpp's bench analysis) ----------

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static uint32_t configured;  // channels set up so far

int adc1_mv(int channel)
{
    if (!adc) {
        adc_oneshot_unit_init_cfg_t ucfg = {};
        ucfg.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) {
            return -1;
        }
        adc_cali_curve_fitting_config_t cc = {};
        cc.unit_id = ADC_UNIT_1;
        cc.atten = ADC_ATTEN_DB_12;
        cc.bitwidth = ADC_BITWIDTH_12;
        adc_cali_create_scheme_curve_fitting(&cc, &cali);
    }
    if (!(configured & (1u << channel))) {
        adc_oneshot_chan_cfg_t ccfg = {};
        ccfg.atten = ADC_ATTEN_DB_12;
        ccfg.bitwidth = ADC_BITWIDTH_12;
        adc_oneshot_config_channel(adc, (adc_channel_t)channel, &ccfg);
        configured |= 1u << channel;
    }
    int raw = 0, out = 0;
    adc_oneshot_read(adc, (adc_channel_t)channel, &raw);
    if (cali && adc_cali_raw_to_voltage(cali, raw, &out) == ESP_OK) {
        return out;
    }
    return raw * 3100 / 4095;
}

// NAN when no divider is configured or fitted.
float battery_volts()
{
#if CONFIG_OBD_VBAT_GPIO >= 1 && CONFIG_OBD_VBAT_GPIO <= 10
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc1_mv(CONFIG_OBD_VBAT_GPIO - 1);  // S3: GPIO1..10 = ADC1 ch0..9
    }
    float v = sum / 16.0f / 1000.0f * CONFIG_OBD_VBAT_RATIO_X1000 / 1000.0f;
    return v < 3.0f ? NAN : v;  // nothing on the pin
#else
    return NAN;
#endif
}

// ---------- sleeping ----------

static void sleep_now(const char *reason)
{
    float v = battery_volts();
    bool vsense = !std::isnan(v);
    bool can_wake = rtc_sleeps >= rtc_can_off_until || !vsense;
    snprintf(rtc_last_reason, sizeof(rtc_last_reason), "%s", reason);
    rtc_sleeps++;

    can_stop();
    led_off();
    // Keep the transceiver's TX input recessive while we're away.
    rtc_gpio_init(CAN_TX_GPIO);
    rtc_gpio_set_direction(CAN_TX_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(CAN_TX_GPIO, 1);
    rtc_gpio_hold_en(CAN_TX_GPIO);

    if (vsense) {
        esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_OBD_SLEEP_CHECK_S * 1000000ULL);
    }
    if (can_wake) {
        esp_sleep_enable_ext0_wakeup(CAN_RX_GPIO, 0);  // dominant bit on the bus
        rtc_gpio_pullup_en(CAN_RX_GPIO);
    }
    if (!vsense && !can_wake) {
        esp_sleep_enable_timer_wakeup(60 * 1000000ULL);  // shouldn't happen; never sleep forever
    }
    esp_deep_sleep_start();
}

void power_early_check()
{
    rtc_gpio_hold_dis(CAN_TX_GPIO);
    rtc_gpio_deinit(CAN_TX_GPIO);
    rtc_gpio_deinit(CAN_RX_GPIO);
    awake_since_us = esp_timer_get_time();

    uint32_t causes = esp_sleep_get_wakeup_causes();
    if ((causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) && rtc_vsense_ok) {
        rtc_checks++;
        float v = battery_volts();
        if (!std::isnan(v) && v < WAKE_VOLTS) {
            // Engine not running: straight back to sleep, no WiFi, ~0.3 s awake.
            rtc_sleeps--;  // not a new sleep, just a check
            sleep_now(rtc_last_reason);
        }
        snprintf(why_awake, sizeof(why_awake), "battery %.1f V (engine started)", v);
    } else if (causes & (1u << ESP_SLEEP_WAKEUP_EXT0)) {
        rtc_can_wakes++;
        woke_by_can = true;
        snprintf(why_awake, sizeof(why_awake), "CAN activity");
    } else {
        snprintf(why_awake, sizeof(why_awake), "power-on or reset");
    }
}

static bool nvs_vsense_ok()
{
    nvs_handle_t h;
    uint8_t ok = 0;
    if (nvs_open("power", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "vsense", &ok);
        nvs_close(h);
    }
    return ok;
}

static void save_vsense_ok()
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vsense", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void power_task(void *)
{
    if (!rtc_vsense_ok) {
        rtc_vsense_ok = nvs_vsense_ok();
    }
    int64_t car_off_since = 0;
    bool answered = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t now = esp_timer_get_time();
        int64_t last_answer = logger_last_answer_us();
        if (last_answer > awake_since_us && !answered) {
            answered = true;
            rtc_noise_streak = 0;  // the car really was there
        }
        float ecu_v = logger_ecu_volts();
        float v_now = battery_volts();
        if (!rtc_vsense_ok && ecu_v > 9 && !std::isnan(v_now) && fabsf(v_now - ecu_v) < 0.8f &&
            now - last_answer < 5000000) {
            rtc_vsense_ok = true;
            save_vsense_ok();
            ESP_LOGI(TAG, "battery sense %.2f V matches the ECU's %.2f V; sleep enabled", v_now, ecu_v);
        }
#if !CONFIG_OBD_SLEEP
        continue;
#endif
        if (!rtc_vsense_ok || power_hold.load() || ota_pending_verify()) {
            continue;
        }
        if (now - awake_since_us < MIN_AWAKE_S * 1000000LL) {
            continue;
        }
        float v = battery_volts();
        bool engine_running = !std::isnan(v) && v >= WAKE_VOLTS;
        bool car_quiet = logger_idle() && now - last_answer > ECU_SILENT_S * 1000000LL;
        if (engine_running || !car_quiet) {
            car_off_since = 0;
            continue;
        }
        if (!car_off_since) {
            car_off_since = now;
        }
        bool uploads_done = pending_trips.load() == 0 && !uploading.load();
        bool grace_over = now - car_off_since > UPLOAD_GRACE_S * 1000000LL;
        if (!(uploads_done || !wifi_up.load() || grace_over)) {
            continue;  // home WiFi and trips still going up
        }
        if (woke_by_can && !answered && ++rtc_noise_streak >= NOISE_WAKES_MAX) {
            rtc_noise_streak = 0;
            rtc_can_off_until = rtc_sleeps + NOISE_BACKOFF_SLEEPS;
            ESP_LOGW(TAG, "CAN wakes without an ECU answering; voltage-only wake for a while");
        }
        ESP_LOGI(TAG, "car off, sleeping (battery %.1f V)", v);
        sleep_now(uploads_done ? "car off, all uploaded" : wifi_up.load() ? "car off, uploads timed out" : "car off, no WiFi");
    }
}

void power_start()
{
    xTaskCreate(power_task, "power", 3072, nullptr, 2, nullptr);
}

void power_status(char *out, size_t len)
{
    float v = battery_volts();
    char volts[16];
    if (std::isnan(v)) {
        snprintf(volts, sizeof(volts), "no sense");
    } else {
        snprintf(volts, sizeof(volts), "%.2f V", v);
    }
    snprintf(out, len, "battery %s%s, awake for %lld s (%s), sleep %s; %lu sleeps, %lu voltage checks, %lu CAN wakes%s%s%s",
             volts, rtc_vsense_ok ? "" : " (not yet confirmed against the ECU)",
             (long long)((esp_timer_get_time() - awake_since_us) / 1000000), why_awake,
             !CONFIG_OBD_SLEEP ? "off" : rtc_vsense_ok ? "on" : "waiting for battery sense", (unsigned long)rtc_sleeps, (unsigned long)rtc_checks,
             (unsigned long)rtc_can_wakes, rtc_sleeps < rtc_can_off_until ? " (CAN wake paused)" : "",
             rtc_last_reason[0] ? "; last: " : "", rtc_last_reason);
}
