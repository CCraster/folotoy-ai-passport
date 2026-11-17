#include "passport.h"

#include <stdio.h>
#include <stdlib.h>
#include "photo_jpeg.h"
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sys/time.h"

static nvs_handle_t settings;
static const esp_partition_t *assets;
static const uint8_t *mapping;
static esp_partition_mmap_handle_t mapped;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static RTC_DATA_ATTR int16_t utc_offset = 480;
static SemaphoreHandle_t settings_lock;
static nvs_handle_t operations;

passport_profile_t profile;
static photo_album_t saved_album;
static passport_language_t saved_language;
static passport_preferences_t saved_preferences = {.brightness = 70, .volume = 40, .sleep_seconds = 60, .deep_sleep_seconds = 300};

_Static_assert(offsetof(passport_preferences_t, deep_sleep_seconds) == 8, "Keep legacy preference offsets");
_Static_assert(offsetof(passport_preferences_t, power_mode) == 6, "Keep the display tag byte");
_Static_assert(sizeof(passport_preferences_t) == 10, "Keep legacy preference blob size");

// 位 7 标记显示扩展；位 6、5 清零时记忆游戏返回页、当前页。旧固件会回到额度页。
// 位 4 记忆额度大字版，位 3–2 保存场景，位 1–0 保留动画页标记。
enum { DISPLAY_BYTE = 7, DISPLAY_TAG = 0xe0, DISPLAY_FOCUS = 0x10,
       CITY_PAGE_CURRENT = 1, CITY_PAGE_RETURN = 2,
       DISPLAY_EXTENDED = 0x80, GAME_PAGE_CURRENT = 0x20, GAME_PAGE_RETURN = 0x40 };
_Static_assert(PIXEL_SCENE_COUNT <= 3, "Display tag has three scene values");

static void decode_display(passport_preferences_t *settings, uint8_t tag, size_t size)
{
    settings->scene = PIXEL_STREET;
    settings->quota_focus = false;
    unsigned scene_code = (tag >> 2) & 3;
    if (size != sizeof *settings || !(tag & DISPLAY_EXTENDED) ||
        scene_code == 0 || scene_code > PIXEL_SCENE_COUNT) return;
    settings->scene = scene_code - 1;
    settings->quota_focus = (tag & DISPLAY_FOCUS) != 0;
    if ((tag & CITY_PAGE_CURRENT) && settings->page == QUOTA) settings->page = CITY;
    if ((tag & CITY_PAGE_RETURN) && settings->return_page == QUOTA) settings->return_page = CITY;
    if (!(tag & GAME_PAGE_CURRENT) && settings->page == QUOTA) settings->page = GAME;
    if (!(tag & GAME_PAGE_RETURN) && settings->return_page == QUOTA) settings->return_page = GAME;
}

_Static_assert(offsetof(passport_profile_t, subtitle) == 340, "Keep existing profile fields at their stored offsets");
_Static_assert(BADGE_BYTES <= 0x30000, "Identity image and portrait must fit one asset slot");

enum {
    JPEG_SLOTS = 8, LEGACY_PHOTO_SLOTS = 4,
    PHOTO_SLOT_BYTES = PHOTO_JPEG_LIMIT * 2, BADGE_SLOT_BYTES = 0x30000, ASSETS_BYTES = 0x100000,
};
static const char album_key[] = "photo_album_v2";
static const char legacy_album_key[] = "photo_album_v1";
// 每个旧照片槽拆成两个 JPEG 槽。旧照片迁移前占用整对，身份卡偏移保持原样。
static const uint32_t photo_offsets[] = {0, PHOTO_SLOT_BYTES, 0xb0000, 0xd8000};
_Static_assert(sizeof(photo_offsets) / sizeof(*photo_offsets) == LEGACY_PHOTO_SLOTS, "Keep all legacy offsets");
_Static_assert(0xd8000 + PHOTO_SLOT_BYTES == ASSETS_BYTES, "Photo slots must fit asset partition");
_Static_assert(JPEG_SLOTS > PHOTO_CAPACITY, "Keep a spare photo slot");

static size_t asset_offset(bool photo, uint8_t slot)
{
    if (!photo) return 0x50000 + (slot - 1) * BADGE_SLOT_BYTES;
    if (slot > JPEG_SLOTS) return photo_offsets[slot - JPEG_SLOTS - 1];
    return photo_offsets[(slot - 1) / 2] + ((slot - 1) % 2) * PHOTO_JPEG_LIMIT;
}

static uint16_t occupied_photo_blocks(uint8_t slot)
{
    if (!slot) return 0;
    if (slot <= JPEG_SLOTS) return 1u << (slot - 1);
    return 3u << ((slot - JPEG_SLOTS - 1) * 2);
}

static uint8_t free_photo_slot(const photo_album_t *album)
{
    uint16_t occupied = 0;
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i) occupied |= occupied_photo_blocks(album->slots[i]);
    for (uint8_t slot = 1; slot <= JPEG_SLOTS; ++slot)
        if (!(occupied & occupied_photo_blocks(slot))) return slot;
    return 0;
}

static esp_err_t erase_asset(bool photo, uint8_t slot)
{
    size_t length = photo ? (slot <= JPEG_SLOTS ? PHOTO_JPEG_LIMIT : PHOTO_SLOT_BYTES) : BADGE_SLOT_BYTES;
    return esp_partition_erase_range(assets, asset_offset(photo, slot), length);
}

static esp_err_t load_album(void)
{
    saved_album = (photo_album_t){0};
    size_t size = sizeof saved_album;
    esp_err_t err = nvs_get_blob(settings, album_key, &saved_album, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        struct { uint8_t slots[3]; uint8_t selected; } old = {0};
        size = sizeof old;
        err = nvs_get_blob(settings, legacy_album_key, &old, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            old.slots[0] = profile.photo <= 2 ? profile.photo : 0;
        } else if (err != ESP_OK || size != sizeof old || old.selected >= 3) {
            return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
        }
        saved_album.selected = old.selected;
        for (unsigned i = 0; i < 3; ++i) {
            if (old.slots[i] > LEGACY_PHOTO_SLOTS) return ESP_ERR_INVALID_ARG;
            if (!old.slots[i]) continue;
            saved_album.slots[i] = old.slots[i] + JPEG_SLOTS;
            saved_album.bytes[i] = PHOTO_BYTES;
        }
    } else if (err != ESP_OK || size != sizeof saved_album) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    if (saved_album.selected >= PHOTO_CAPACITY) return ESP_ERR_INVALID_ARG;
    uint16_t used = 0;
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i) {
        const uint8_t slot = saved_album.slots[i];
        const uint32_t bytes = saved_album.bytes[i];
        if (!slot && !bytes) continue;
        if (!slot || !bytes || slot > JPEG_SLOTS + LEGACY_PHOTO_SLOTS ||
            (slot > JPEG_SLOTS ? bytes != PHOTO_BYTES : bytes > PHOTO_JPEG_LIMIT)) return ESP_ERR_INVALID_ARG;
        const uint16_t blocks = occupied_photo_blocks(slot);
        if (used & blocks) return ESP_ERR_INVALID_ARG;
        used |= blocks;
    }
    if (used && !saved_album.slots[saved_album.selected]) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static void migrate_legacy_photos(void);

static esp_err_t clear_namespace(const char *name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// 重启后、任何显示或通信任务启动前执行清除。标记最后删除，断电后可继续。
static esp_err_t resume_maintenance(void)
{
    uint8_t action;
    esp_err_t err = nvs_get_u8(operations, "pending", &action);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (action != MAINTENANCE_FORGET_MAC && action != MAINTENANCE_RESET) return ESP_ERR_INVALID_ARG;
    if (action == MAINTENANCE_RESET) {
        if (assets->address != 0x500000 || assets->size != 0x100000) return ESP_ERR_INVALID_SIZE;
        err = esp_partition_erase_range(assets, 0, assets->size);
        if (err == ESP_OK) err = clear_namespace("passport_v1");
    }
    // ESP-IDF 5.5.3 NimBLE 将配对记录保存在此独立命名空间。
    if (err == ESP_OK) err = clear_namespace("nimble_bond");
    if (err == ESP_OK) err = nvs_erase_key(operations, "pending");
    if (err == ESP_OK) err = nvs_commit(operations);
    return err;
}

esp_err_t schedule_maintenance(uint8_t action)
{
    if (action != MAINTENANCE_FORGET_MAC && action != MAINTENANCE_RESET) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_u8(operations, "pending", action);
    return err == ESP_OK ? nvs_commit(operations) : err;
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
    if (!assets || assets->size < ASSETS_BYTES) return ESP_ERR_NOT_FOUND;
    err = nvs_open("passport_ops", NVS_READWRITE, &operations);
    if (err != ESP_OK) return err;
    err = resume_maintenance();
    if (err != ESP_OK) return err;
    settings_lock = xSemaphoreCreateMutex();
    if (!settings_lock) return ESP_ERR_NO_MEM;
    // A separate namespace leaves the original firmware's NVS entries available.
    err = nvs_open("passport_v1", NVS_READWRITE, &settings);
    if (err != ESP_OK) return err;
    // 独立键保留旧版 preferences 的二进制布局。
    uint8_t language = PASSPORT_ZH;
    err = nvs_get_u8(settings, "language", &language);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    saved_language = language == PASSPORT_EN ? PASSPORT_EN : PASSPORT_ZH;
    size_t size = sizeof profile;
    err = nvs_get_blob(settings, "profile", &profile, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    err = load_album();
    if (err != ESP_OK) return err;
    uint8_t blob[sizeof saved_preferences];
    size = sizeof blob;
    err = nvs_get_blob(settings, "preferences", blob, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    // 旧版 8 字节配置保留所有字段，新增休眠计时沿用默认五分钟。
    if (err == ESP_OK) {
        if (size != 8 && size != sizeof saved_preferences) return ESP_ERR_INVALID_SIZE;
        memcpy(&saved_preferences, blob, size);
        decode_display(&saved_preferences, blob[DISPLAY_BYTE], size);
    }
    passport_preferences_t checked;
    if (!preferences_merge(&checked, &saved_preferences, &saved_preferences, PREF_DEVICE | PREF_PAGE))
        return ESP_ERR_INVALID_ARG;
    const void *address;
    err = esp_partition_mmap(assets, 0, assets->size, ESP_PARTITION_MMAP_DATA, &address, &mapped);
    if (err == ESP_OK) {
        mapping = address;
        migrate_legacy_photos();
    }
    return err;
}

static esp_err_t store_blob(const char *key, const void *data, size_t size)
{
    esp_err_t err = nvs_set_blob(settings, key, data, size);
    return err == ESP_OK ? nvs_commit(settings) : err;
}

passport_language_t passport_language(void)
{
    portENTER_CRITICAL(&state_lock);
    passport_language_t language = saved_language;
    portEXIT_CRITICAL(&state_lock);
    return language;
}

esp_err_t save_language(passport_language_t language)
{
    if (language != PASSPORT_ZH && language != PASSPORT_EN) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (language != passport_language()) {
        err = nvs_set_u8(settings, "language", language);
        if (err == ESP_OK) err = nvs_commit(settings);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&state_lock);
            saved_language = language;
            portEXIT_CRITICAL(&state_lock);
        }
    }
    xSemaphoreGive(settings_lock);
    return err;
}

passport_preferences_t preferences_snapshot(void)
{
    portENTER_CRITICAL(&state_lock);
    passport_preferences_t result = saved_preferences;
    portEXIT_CRITICAL(&state_lock);
    return result;
}

esp_err_t update_preferences(unsigned mask, const passport_preferences_t *value)
{
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    passport_preferences_t next;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    // 按字段合并，网页调节设备设置时，翻页操作只更新页面记忆。
    if (preferences_merge(&next, &saved_preferences, value, mask)) {
        passport_preferences_t compatible = next;
        uint8_t city = 0;
        if (compatible.page == CITY) { compatible.page = QUOTA; city |= CITY_PAGE_CURRENT; }
        if (compatible.return_page == CITY) { compatible.return_page = QUOTA; city |= CITY_PAGE_RETURN; }
        uint8_t tag = DISPLAY_TAG;
        if (compatible.page == GAME) { compatible.page = QUOTA; tag &= ~GAME_PAGE_CURRENT; }
        if (compatible.return_page == GAME) { compatible.return_page = QUOTA; tag &= ~GAME_PAGE_RETURN; }
        uint8_t blob[sizeof compatible];
        memcpy(blob, &compatible, sizeof blob);
        blob[DISPLAY_BYTE] = tag != DISPLAY_TAG || city || next.scene != PIXEL_STREET || next.quota_focus
            ? tag | (next.quota_focus ? DISPLAY_FOCUS : 0) | ((next.scene + 1) << 2) | city : 0;
        err = store_blob("preferences", blob, sizeof blob);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&state_lock);
            saved_preferences = next;
            portEXIT_CRITICAL(&state_lock);
        }
    }
    xSemaphoreGive(settings_lock);
    return err;
}

// 无尽纪录按米保存，原版鱼干数留在原键，兼容回退。
uint32_t load_meow_best(void)
{
    uint32_t meters;
    esp_err_t err = nvs_get_u32(settings, "meow_distance", &meters);
    if (err == ESP_OK) return meters;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW("passport", "Load game distance: %s", esp_err_to_name(err));
    return 0;
}

esp_err_t save_meow_best(uint32_t meters)
{
    esp_err_t err = nvs_set_u32(settings, "meow_distance", meters);
    return err == ESP_OK ? nvs_commit(settings) : err;
}

bool load_meow_tutorial(void)
{
    uint8_t done;
    esp_err_t err = nvs_get_u8(settings, "meow_tutorial", &done);
    if (err == ESP_OK) return done == 1;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW("passport", "Load game tutorial: %s", esp_err_to_name(err));
    return false;
}

esp_err_t save_meow_tutorial(void)
{
    esp_err_t err = nvs_set_u8(settings, "meow_tutorial", 1);
    return err == ESP_OK ? nvs_commit(settings) : err;
}

typedef struct { char ssid[33]; char password[65]; } wifi_credentials_t;

esp_err_t load_wifi(char ssid[33], char password[65])
{
    wifi_credentials_t saved = {0};
    size_t size = sizeof saved;
    esp_err_t err = nvs_get_blob(settings, "wifi", &saved, &size);
    if (err != ESP_OK) return err;
    memcpy(ssid, saved.ssid, sizeof saved.ssid);
    memcpy(password, saved.password, sizeof saved.password);
    return ESP_OK;
}

esp_err_t save_wifi(const char *ssid, const char *password)
{
    wifi_credentials_t saved = {0};
    snprintf(saved.ssid, sizeof saved.ssid, "%s", ssid);
    snprintf(saved.password, sizeof saved.password, "%s", password);
    return store_blob("wifi", &saved, sizeof saved);
}

// 独立键兼容原 preferences 的布局，关闭无线时保留已保存的网络凭据。
esp_err_t load_wifi_enabled(bool *enabled)
{
    uint8_t disabled = 0;
    esp_err_t err = nvs_get_u8(settings, "wifi_off", &disabled);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (disabled > 1) return ESP_ERR_INVALID_ARG;
    *enabled = !disabled;
    return ESP_OK;
}

esp_err_t save_wifi_enabled(bool enabled)
{
    esp_err_t err = nvs_set_u8(settings, "wifi_off", !enabled);
    return err == ESP_OK ? nvs_commit(settings) : err;
}

// 单独保存开关，兼容旧偏好结构；关闭蓝牙保留 NimBLE 配对记录。
esp_err_t load_ble_enabled(bool *enabled)
{
    uint8_t disabled = 0;
    esp_err_t err = nvs_get_u8(settings, "ble_off", &disabled);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (disabled > 1) return ESP_ERR_INVALID_ARG;
    *enabled = !disabled;
    return ESP_OK;
}

esp_err_t save_ble_enabled(bool enabled)
{
    esp_err_t err = nvs_set_u8(settings, "ble_off", !enabled);
    return err == ESP_OK ? nvs_commit(settings) : err;
}

passport_profile_t profile_snapshot(void)
{
    portENTER_CRITICAL(&state_lock);
    passport_profile_t result = profile;
    portEXIT_CRITICAL(&state_lock);
    return result;
}

photo_album_t photo_album_snapshot(void)
{
    portENTER_CRITICAL(&state_lock);
    photo_album_t result = saved_album;
    portEXIT_CRITICAL(&state_lock);
    return result;
}

// 调用方持有 settings_lock，闪存写入成功后才发布新相册。
static esp_err_t save_album(const photo_album_t *next)
{
    esp_err_t err = store_blob(album_key, next, sizeof *next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&state_lock);
        saved_album = *next;
        portEXIT_CRITICAL(&state_lock);
    }
    return err;
}

// 启动时逐张转码。新槽校验和目录提交完成前，原 RGB565 像素始终可读。
static void migrate_legacy_photos(void)
{
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i) {
        uint8_t old_slot = saved_album.slots[i];
        if (old_slot <= JPEG_SLOTS) continue;
        uint8_t slot = free_photo_slot(&saved_album);
        uint8_t *jpeg = slot ? malloc(PHOTO_JPEG_LIMIT) : NULL;
        if (!jpeg) {
            ESP_LOGW("storage", "Photo %u migration deferred: no buffer or slot", i + 1);
            return;
        }
        size_t size = photo_jpeg_from_rgb565(asset_data(true, old_slot), jpeg);
        esp_err_t err = size ? erase_asset(true, slot) : ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK) err = esp_partition_write(assets, asset_offset(true, slot), jpeg, size);
        if (err == ESP_OK && memcmp(asset_data(true, slot), jpeg, size)) err = ESP_ERR_INVALID_CRC;
        free(jpeg);
        if (err == ESP_OK) {
            photo_album_t next = saved_album;
            next.slots[i] = slot;
            next.bytes[i] = size;
            err = save_album(&next);
        }
        if (err == ESP_OK) {
            ESP_LOGI("storage", "Photo %u migrated: %u bytes, selected=%u", i + 1, (unsigned)size, saved_album.selected + 1);
        } else {
            ESP_LOGW("storage", "Photo %u migration kept original: %s", i + 1, esp_err_to_name(err));
        }
    }
}

static unsigned next_photo_index(const photo_album_t *album)
{
    for (unsigned step = 1; step <= PHOTO_CAPACITY; ++step) {
        unsigned index = (album->selected + step) % PHOTO_CAPACITY;
        if (album->slots[index]) return index;
    }
    return 0;
}

esp_err_t select_next_photo(void)
{
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    photo_album_t next = saved_album;
    next.selected = next_photo_index(&next);
    esp_err_t err = next.selected == saved_album.selected ? ESP_ERR_NOT_FOUND : save_album(&next);
    xSemaphoreGive(settings_lock);
    return err;
}

esp_err_t select_photo(unsigned index)
{
    if (index >= PHOTO_CAPACITY) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    photo_album_t next = saved_album;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (next.slots[index]) {
        err = ESP_OK;
        if (next.selected != index) {
            next.selected = index;
            err = save_album(&next);
        }
    }
    xSemaphoreGive(settings_lock);
    return err;
}

const uint8_t *asset_data(bool photo, uint8_t slot)
{
    if (!slot || slot > (photo ? JPEG_SLOTS + LEGACY_PHOTO_SLOTS : 2)) return NULL;
    return mapping + asset_offset(photo, slot);
}

size_t photo_asset_size(uint8_t slot)
{
    if (!slot) return 0;
    photo_album_t album = photo_album_snapshot();
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i)
        if (album.slots[i] == slot) return album.bytes[i];
    return 0;
}

static void clear_removed_asset(bool photo, uint8_t slot)
{
    if (slot && detach_display_asset(asset_data(photo, slot))) {
        esp_err_t err = erase_asset(photo, slot);
        if (err != ESP_OK) ESP_LOGW("storage", "Clear unused image slot: %s", esp_err_to_name(err));
    }
}

esp_err_t remove_photo(unsigned index)
{
    if (index >= PHOTO_CAPACITY) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    photo_album_t next = saved_album;
    uint8_t previous = next.slots[index];
    if (!previous) {
        xSemaphoreGive(settings_lock);
        return ESP_OK;
    }
    next.slots[index] = 0;
    next.bytes[index] = 0;
    if (next.selected == index) next.selected = next_photo_index(&next);
    esp_err_t err = save_album(&next);
    xSemaphoreGive(settings_lock);
    if (err == ESP_OK) clear_removed_asset(true, previous);
    return err;
}

static esp_err_t write_asset(httpd_req_t *req, bool photo, uint8_t slot, uint32_t checksum)
{
    size_t offset = asset_offset(photo, slot);
    size_t length = req->content_len;
    if (!detach_display_asset(asset_data(photo, slot))) return ESP_ERR_TIMEOUT;
    esp_err_t err = erase_asset(photo, slot);
    if (err != ESP_OK) return err;

    // 先写备用槽并读回校验。在此期间旧照片仍由相册目录引用。
    uint8_t buffer[1024];
    uint32_t crc = 0;
    int64_t deadline = esp_timer_get_time() + 20000000;
    for (size_t received = 0; received < length;) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        size_t remaining = length - received;
        int count = httpd_req_recv(req, (char *)buffer, remaining < sizeof buffer ? remaining : sizeof buffer);
        if (count <= 0) return ESP_ERR_TIMEOUT;
        crc = esp_rom_crc32_le(crc, buffer, count);
        err = esp_partition_write(assets, offset + received, buffer, count);
        if (err != ESP_OK) return err;
        received += count;
    }
    const uint32_t readback = photo || crc == checksum
        ? esp_rom_crc32_le(0, asset_data(photo, slot), length) : 0;
    if (photo) ESP_LOGI("photo_diag", "crc received_ok=%d readback_ok=%d",
                        crc == checksum, readback == checksum);
    if (crc != checksum || readback != checksum) return ESP_ERR_INVALID_CRC;
    return ESP_OK;
}

esp_err_t receive_photo(httpd_req_t *req, unsigned index, uint32_t checksum)
{
    if (index >= PHOTO_CAPACITY) return ESP_ERR_INVALID_ARG;
    const size_t size = req->content_len;
    if (!size || size > PHOTO_JPEG_LIMIT) return ESP_ERR_INVALID_SIZE;
    // HTTP 任务串行修改照片槽位。上传时短按确认仍可换图。
    photo_album_t album = photo_album_snapshot();
    uint8_t slot = free_photo_slot(&album);
    if (!slot) return ESP_ERR_INVALID_STATE;
    esp_err_t err = write_asset(req, true, slot, checksum);
    if (err != ESP_OK) return err;
    err = photo_jpeg_validate_upload(asset_data(true, slot), size);
    if (err != ESP_OK) return err;

    xSemaphoreTake(settings_lock, portMAX_DELAY);
    album = saved_album;
    uint8_t previous = album.slots[index];
    if (!album.slots[album.selected]) album.selected = index;
    album.slots[index] = slot;
    album.bytes[index] = size;
    err = save_album(&album);
    xSemaphoreGive(settings_lock);
    if (err == ESP_OK) clear_removed_asset(true, previous);
    return err;
}

static esp_err_t save_profile(const passport_profile_t *next)
{
    esp_err_t err = store_blob("profile", next, sizeof *next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&state_lock);
        profile = *next;
        portEXIT_CRITICAL(&state_lock);
    }
    return err;
}

esp_err_t remove_badge(void)
{
    passport_profile_t current = profile_snapshot();
    if (!current.badge) return ESP_OK;
    // 保留旧照片迁移指针，清除身份卡的内容与槽位。
    passport_profile_t next = {.photo = current.photo};
    esp_err_t err = save_profile(&next);
    if (err == ESP_OK) clear_removed_asset(false, current.badge);
    return err;
}

esp_err_t receive_badge(httpd_req_t *req, uint32_t checksum,
                        const char *name, const char *subtitle, const char *bio)
{
    size_t length = req->content_len;
    if (length != BADGE_BYTES && length != LEGACY_BADGE_BYTES) return ESP_ERR_INVALID_SIZE;
    passport_profile_t next = profile_snapshot();
    uint8_t slot = next.badge == 1 ? 2 : 1;
    esp_err_t err = write_asset(req, false, slot, checksum);
    if (err != ESP_OK) return err;
    next.badge = slot;
    next.portrait = length == BADGE_BYTES;
    snprintf(next.name, sizeof next.name, "%s", name);
    if (subtitle) snprintf(next.subtitle, sizeof next.subtitle, "%s", subtitle);
    snprintf(next.bio, sizeof next.bio, "%s", bio);
    return save_profile(&next);
}

void set_clock(uint32_t epoch, int16_t offset)
{
    if (epoch < 1700000000 || offset < -720 || offset > 840) return;
    const struct timeval value = {.tv_sec = epoch};
    settimeofday(&value, NULL);
    portENTER_CRITICAL(&state_lock);
    utc_offset = offset;
    portEXIT_CRITICAL(&state_lock);
}

struct tm local_clock(time_t epoch)
{
    portENTER_CRITICAL(&state_lock);
    epoch += utc_offset * 60;
    portEXIT_CRITICAL(&state_lock);
    struct tm result;
    gmtime_r(&epoch, &result);
    return result;
}
