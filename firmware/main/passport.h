#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define WIDTH 240
#define HEIGHT 320
#define HEADER_HEIGHT 40
#define AVATAR_WIDTH 112
#define AVATAR_HEIGHT 176
#define LEGACY_AVATAR_SIZE 80
#define PHOTO_BYTES (WIDTH * HEIGHT * 2)
#define PHOTO_CAPACITY 3
#define BADGE_PIXELS (WIDTH * (HEIGHT - HEADER_HEIGHT) * 2)
#define BADGE_BYTES (BADGE_PIXELS + AVATAR_WIDTH * AVATAR_HEIGHT * 2)
#define LEGACY_BADGE_BYTES (BADGE_PIXELS + LEGACY_AVATAR_SIZE * LEGACY_AVATAR_SIZE * 2)

typedef struct {
    char name[97];
    char bio[241];
    uint8_t badge;
    uint8_t photo; // 旧版单张照片的槽位，仅用于相册迁移。
    // 追加字段以兼容已有 NVS 数据中的名字、简介及图片槽位。
    char subtitle[121];
    uint8_t portrait;
} passport_profile_t;

typedef struct {
    uint8_t slots[PHOTO_CAPACITY];
    uint8_t selected;
} photo_album_t;

typedef struct {
    uint8_t page;
    uint8_t brightness;
    uint8_t volume;
    uint8_t return_page;
    uint16_t sleep_seconds;
    uint8_t power_mode;
    uint8_t scene : 2;
    bool quota_focus : 1;
    uint16_t deep_sleep_seconds; // 随身模式的独立休眠计时，与息屏时间分离。
} passport_preferences_t;

enum { POWER_DESKTOP, POWER_PORTABLE };
// 这些值参与页面记忆。显示顺序独立于存储编号。
enum { BADGE, PHOTO, QUOTA, SETTINGS, CITY };
typedef enum { PIXEL_STREET, PIXEL_PARK, PIXEL_HARBOR, PIXEL_SCENE_COUNT } pixel_scene_t;
enum { PREF_PAGE = 1, PREF_BRIGHTNESS = 2, PREF_VOLUME = 4, PREF_SLEEP = 8, PREF_POWER = 16, PREF_DEEP_SLEEP = 32,
       PREF_SCENE = 64, PREF_QUOTA_VIEW = 128,
       PREF_DEVICE = PREF_BRIGHTNESS | PREF_VOLUME | PREF_SLEEP | PREF_POWER | PREF_DEEP_SLEEP };
enum { MAINTENANCE_FORGET_MAC = 1, MAINTENANCE_RESET = 2 };

typedef struct {
    uint16_t minutes;
    uint8_t remaining;
    uint32_t reset_at;
} quota_window_t;

typedef struct {
    uint32_t updated_at;
    quota_window_t windows[2];
} quota_snapshot_t;

typedef struct {
    uint32_t updated_at;
    uint32_t expires_at;
    uint16_t count;
    bool known;
    bool complete;
} reset_snapshot_t;

typedef struct {
    uint8_t active;
    bool tasks_known;
    bool quota_failed;
    bool completed;
} sync_status_t;

#define SYNC_STATUS_BYTES 3
bool decode_sync_status(const uint8_t *packet, size_t length, sync_status_t *out);

typedef struct {
    uint32_t total;
    uint32_t firmware;
    uint32_t firmware_capacity;
    uint32_t assets_capacity;
    uint32_t badge;
    uint32_t photo;
} storage_usage_t;

esp_err_t storage_usage(storage_usage_t *usage);
bool decode_reset_packet(const uint8_t *packet, size_t length, reset_snapshot_t *out);

typedef struct {
    bool active;
    bool hotspot;
    bool wifi_connected;
    bool client;
    char ssid[33];
    char password[17];
    char url[80];
    char short_url[40];
    char code[7];
} web_status_t;

extern passport_profile_t profile;

esp_err_t storage_init(void);
passport_preferences_t preferences_snapshot(void);
esp_err_t update_preferences(unsigned mask, const passport_preferences_t *value);
bool preferences_merge(passport_preferences_t *out, const passport_preferences_t *current,
                       const passport_preferences_t *value, unsigned mask);
esp_err_t schedule_maintenance(uint8_t action);
esp_err_t load_wifi(char ssid[33], char password[65]);
esp_err_t save_wifi(const char *ssid, const char *password);
esp_err_t load_wifi_enabled(bool *enabled);
esp_err_t save_wifi_enabled(bool enabled);
esp_err_t load_ble_enabled(bool *enabled);
esp_err_t save_ble_enabled(bool enabled);
passport_profile_t profile_snapshot(void);
photo_album_t photo_album_snapshot(void);
// 没有另一张可展示的照片时返回 ESP_ERR_NOT_FOUND。
esp_err_t select_next_photo(void);
esp_err_t select_photo(unsigned index);
esp_err_t remove_photo(unsigned index);
esp_err_t receive_photo(httpd_req_t *req, unsigned index, uint32_t checksum);
const uint8_t *asset_data(bool photo, uint8_t slot);
bool detach_display_asset(const uint8_t *pixels);
esp_err_t remove_badge(void);
esp_err_t receive_badge(httpd_req_t *req, uint32_t checksum,
                        const char *name, const char *subtitle, const char *bio);
void set_clock(uint32_t epoch, int16_t offset);
struct tm local_clock(time_t epoch);

void web_init(void);
esp_err_t web_open(bool force_hotspot);
void web_poll(void);
bool web_expired(void);
void web_close(void);
esp_err_t web_shutdown(void);
web_status_t web_status(void);

void quota_ble_init(void);
esp_err_t quota_ble_stop(void);
esp_err_t quota_ble_set_enabled(bool enabled);
bool quota_ble_enabled(void);
bool quota_paired(void);
void quota_pair(void);
void quota_end_pairing(void);
bool quota_connected(void);
int quota_passkey(void);
quota_snapshot_t quota_snapshot(void);
reset_snapshot_t quota_resets(void);
sync_status_t quota_sync_status(int64_t *completion_until);
