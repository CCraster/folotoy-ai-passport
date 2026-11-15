#pragma once
#include "esp_err.h"
int bsp_button_read_mv(void);
esp_err_t bsp_button_prepare_deep_sleep(int *level);
