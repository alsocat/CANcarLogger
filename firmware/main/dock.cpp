// Boost gauge output: one text line every 50 ms on a UART TX pin, for an
// iPod running Rockbox with the boost_gauge plugin (dock connector serial,
// 3.3 V, 57600 8N1). Lines look like "B=142 R=3450 C=90 I=35 S=88": boost in
// tenths of psi, rpm, coolant C, intake air C, speed km/h. Nothing is sent
// while the engine data isn't fresh.

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "shared.h"

#define DOCK_UART UART_NUM_1
#define DOCK_BAUD 57600

#if CONFIG_OBD_DOCK_TX_GPIO >= 0
static void dock_task(void *)
{
    char line[64];
    while (true) {
        if (logger_dock_line(line, sizeof(line))) {
            uart_write_bytes(DOCK_UART, line, strlen(line));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void dock_start()
{
#if CONFIG_OBD_DOCK_TX_GPIO >= 0
    uart_config_t cfg = {};
    cfg.baud_rate = DOCK_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(DOCK_UART, 256, 256, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(DOCK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DOCK_UART, CONFIG_OBD_DOCK_TX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(dock_task, "dock", 2560, nullptr, 3, nullptr);
    ESP_LOGI("dock", "boost gauge output on GPIO%d", CONFIG_OBD_DOCK_TX_GPIO);
#endif
}
