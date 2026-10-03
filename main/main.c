// Companion application entry. The baseline demo sources stay in the tree for
// their host tests; this firmware does not link or open that menu.
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "companion_store.h"
#include "companion_ui.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "companion start");
    companion_err_t store_err = companion_store_mount();
    if (store_err != COMPANION_OK) {
        ESP_LOGE(TAG, "storage mount failed: %d", (int)store_err);
    }

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed mosi=%d sclk=%d cs=%d dc=%d bl=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(80);
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery init failed");
    }
    companion_ui_start();
}
