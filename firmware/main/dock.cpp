// Car gauges on an iPod running Rockbox with the boost_gauge plugin, plugged
// into the board's USB OTG port. The board is the USB host: while the gauges
// are open, Rockbox shows up as a USB serial (CDC-ACM) device and gets
//   - every 50 ms while the engine data is fresh, a line of live values
//     ("B=142 R=3450 S=88 ..."; letters and units in the plugin's header)
//   - every 2 s, "@D ..." with the MIL and the last health scan's codes
// and the iPod sends "SCAN" to ask for a new health scan. Plug and unplug
// whenever.
//
// The port only powers the iPod if its 5 V comes from the board: the DevKitC's
// USB-OTG solder jumper, ideally through a switch on OBD_USB_VBUS_GPIO so the
// iPod isn't charged from the car battery while the board sleeps.

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "shared.h"

#ifdef CONFIG_OBD_IPOD_USB
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "dock";
static volatile bool gone;

static void usb_lib_task(void *)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static bool on_rx(const uint8_t *data, size_t len, void *)
{
    static char buf[16];
    static size_t n;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n' || data[i] == '\r') {
            buf[n] = 0;
            if (strcmp(buf, "SCAN") == 0) {
                ESP_LOGI(TAG, "iPod asked for a health scan");
                scan_requested.store(true);
            }
            n = 0;
        } else if (n < sizeof(buf) - 1) {
            buf[n++] = data[i];
        }
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        gone = true;
    }
}

static void dock_task(void *)
{
    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 256,
        .in_buffer_size = 64,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = nullptr,
    };
    char line[160], codes[448];
    while (true) {
        // Waits up to a second for something to be plugged in. Rockbox's
        // serial interface is the first one while it's in charge-only mode.
        cdc_acm_dev_hdl_t dev = nullptr;
        if (cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &dev_cfg, &dev) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG, "iPod connected");
        gone = false;
        cdc_acm_host_set_control_line_state(dev, true, false);
        for (int tick = 0; !gone; tick++) {
            if (logger_dock_line(line, sizeof(line)) &&
                cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)line, strlen(line), 200) != ESP_OK) {
                break;
            }
            if (tick % 40 == 0) {
                logger_dock_codes(codes, sizeof(codes));
                if (cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)codes, strlen(codes), 500) != ESP_OK) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        cdc_acm_host_close(dev);
        ESP_LOGI(TAG, "iPod disconnected");
    }
}
#endif

void dock_start()
{
#ifdef CONFIG_OBD_IPOD_USB
#if CONFIG_OBD_USB_VBUS_GPIO >= 0
    gpio_reset_pin((gpio_num_t)CONFIG_OBD_USB_VBUS_GPIO);
    gpio_set_direction((gpio_num_t)CONFIG_OBD_USB_VBUS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_OBD_USB_VBUS_GPIO, 1);
#endif
    usb_host_config_t host_cfg = {};
    host_cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr);
    ESP_ERROR_CHECK(cdc_acm_host_install(nullptr));
    xTaskCreate(dock_task, "dock", 4096, nullptr, 3, nullptr);
#endif
}
