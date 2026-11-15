#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "passport.h"

typedef enum { IDLE_KEEP_RUNNING, IDLE_SCREEN_OFF, IDLE_DEEP_SLEEP } idle_action_t;
idle_action_t power_idle_action(const passport_preferences_t *settings, uint32_t seconds,
                                bool screen_off, bool configuring);

void power_init(void);
esp_err_t power_display(bool awake, uint8_t brightness);
// 返回失败表示仍可操作界面；进入外设终止阶段后失败会重启恢复。
esp_err_t power_deep_sleep(void);
