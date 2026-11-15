#pragma once

#include "esp_err.h"
#include "rtttl.h"

#define MUSIC_PRESET_COUNT 3
#define MUSIC_CUSTOM MUSIC_PRESET_COUNT
#define MUSIC_VOLUME_FOLLOW UINT8_MAX

typedef struct {
    uint8_t enabled;
    uint8_t selection; // 0..2 为内置旋律，3 为自定义乐谱。
    char custom[RTTTL_MAX_BYTES + 1];
} boot_music_config_t;

typedef struct { bool enabled; uint8_t selection; bool has_custom; } boot_music_status_t;
typedef struct { const char *name; const char *rtttl; } music_preset_t;
extern const music_preset_t music_presets[MUSIC_PRESET_COUNT];

esp_err_t music_init(uint8_t volume);
boot_music_config_t music_config(void);
boot_music_status_t music_status(void);
esp_err_t music_save(const boot_music_config_t *config);
bool music_validate(const boot_music_config_t *config, music_score_t *score);
esp_err_t music_preview(const boot_music_config_t *config);
esp_err_t music_toggle(void);
esp_err_t music_next(void);
// 机身操作在音乐模块内合并字段，设置分发函数无需保留整份乐谱。
esp_err_t music_select(uint8_t selection);
esp_err_t music_preview_current(void);
// 试听指定旋律一次，保留已保存的选择与开机播放开关。
esp_err_t music_preview_selection(uint8_t selection);
void music_boot(void);
bool music_completion_enabled(void);
esp_err_t music_set_completion_enabled(bool enabled);
uint8_t music_completion_volume(void);
esp_err_t music_set_completion_volume(uint8_t volume);
esp_err_t music_preview_completion(void);
void music_completion_chime(void);
void music_stop(void);
void music_set_volume(uint8_t volume);
uint8_t music_volume(void);
void music_volume_sample(void);
// 终端操作：等待播放退出并关闭 I2S，之后必须深睡或重启。
esp_err_t music_prepare_deep_sleep(void);
