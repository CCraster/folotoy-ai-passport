#include "power.h"
#include "passport.h"
#include "music.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"

void power_init(void)
{
    // 桌面模式通过动态调频节电，保持 BLE 链路正常响应。
    const esp_pm_config_t config = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = false};
    ESP_ERROR_CHECK(esp_pm_configure(&config));
}

idle_action_t power_idle_action(const passport_preferences_t *settings, uint32_t seconds,
                                bool screen_off, bool configuring)
{
    if (configuring) return IDLE_KEEP_RUNNING;
    if (settings->power_mode == POWER_PORTABLE && seconds >= settings->deep_sleep_seconds)
        return IDLE_DEEP_SLEEP;
    if (!screen_off && settings->sleep_seconds && seconds >= settings->sleep_seconds)
        return IDLE_SCREEN_OFF;
    return IDLE_KEEP_RUNNING;
}

esp_err_t power_display(bool awake, uint8_t brightness)
{
    if (!bsp_lvgl_lock(500)) return ESP_ERR_TIMEOUT;
    if (!awake) bsp_display_backlight(0);
    esp_err_t err = esp_lcd_panel_disp_on_off(bsp_display_panel(), awake);
    if (awake && err == ESP_OK) bsp_display_backlight(brightness);
    bsp_lvgl_unlock();
    return err;
}

static void terminal_check(const char *step, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE("power", "%s: %s; restart to restore peripherals", step, esp_err_to_name(err));
        esp_restart();
    }
}

esp_err_t power_deep_sleep(void)
{
    int mv = bsp_button_read_mv();
    if (mv < 0) return ESP_FAIL;
    if (mv < BSP_BTN_MV_RELEASED_MIN) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) return err;
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    // 从这里开始只进入深睡或重启，避免返回一个按键/总线已被释放的界面。
    terminal_check("Wi-Fi stop", web_shutdown());
    terminal_check("BLE stop", quota_ble_stop());
    terminal_check("Audio stop", music_prepare_deep_sleep());
    terminal_check("Battery suspend", bsp_battery_sleep());
    terminal_check("I2C release", bsp_i2c_prepare_deep_sleep());
    int level = 0;
    terminal_check("Button handoff", bsp_button_prepare_deep_sleep(&level));
    if (level != 1) {
        ESP_LOGW("power", "Key pressed during sleep preparation; restart to restore input");
        esp_restart();
    }
    terminal_check("Display suspend", bsp_display_prepare_deep_sleep());
    ESP_LOGI("power", "Deep sleep; wake GPIO=%d level=%d", BSP_BTN_GPIO, level);
    esp_deep_sleep_start();
    esp_restart();
    return ESP_FAIL;
}
