#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
bool bsp_lvgl_lock(int timeout);
void bsp_lvgl_unlock(void);
void bsp_display_backlight(uint8_t value);
void *bsp_display_panel(void);
esp_err_t bsp_display_prepare_deep_sleep(void);
