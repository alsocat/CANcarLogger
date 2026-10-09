// Status colour on the DevKitC-1's WS2812 RGB LED:
//   red    - not on WiFi
//   blue   - uploading trips
//   yellow - on WiFi, trips waiting but the server isn't taking them
//   green  - on WiFi and everything is uploaded
// The LED is on GPIO48 (board v1.0) or GPIO38 (v1.1); both are driven.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "shared.h"

std::atomic<bool> uploading{false};
std::atomic<int> pending_trips{0};
static std::atomic<bool> dark{false};

void led_off()
{
    dark.store(true);
    vTaskDelay(pdMS_TO_TICKS(150));  // let the LED task blank it
}

static const int LED_PINS[] = {48, 38};
static const uint8_t BRIGHT = 24;  // out of 255; it's a status glow, not a torch

static void led_task(void *)
{
    led_strip_handle_t strips[2] = {};
    for (int i = 0; i < 2; i++) {
        led_strip_config_t cfg = {};
        cfg.strip_gpio_num = LED_PINS[i];
        cfg.max_leds = 1;
        cfg.led_model = LED_MODEL_WS2812;
        cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        led_strip_rmt_config_t rmt = {};
        rmt.clk_src = RMT_CLK_SRC_DEFAULT;
        rmt.resolution_hz = 10 * 1000 * 1000;
        led_strip_new_rmt_device(&cfg, &rmt, &strips[i]);
    }

    uint32_t last = 0xFFFFFFFF;
    while (true) {
        uint8_t r = 0, g = 0, b = 0;
        if (dark.load()) {
        } else if (!wifi_up.load()) {
            r = BRIGHT;
        } else if (uploading.load()) {
            b = BRIGHT;
        } else if (pending_trips.load() > 0) {
            r = BRIGHT;
            g = BRIGHT * 2 / 3;
        } else {
            g = BRIGHT;
        }
        uint32_t rgb = (r << 16) | (g << 8) | b;
        if (rgb != last) {
            for (auto s : strips) {
                if (s) {
                    led_strip_set_pixel(s, 0, r, g, b);
                    led_strip_refresh(s);
                }
            }
            last = rgb;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_start()
{
    xTaskCreate(led_task, "led", 3072, nullptr, 2, nullptr);
}
