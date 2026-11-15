#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/main/storage.c"

static esp_partition_t flash_partition = {.address = 0x500000, .size = 0x100000};
static uint8_t flash[0x100000];
static bool pending_exists;
static uint8_t pending_action;
static unsigned cleared_user, cleared_bonds, erased_assets;
static esp_err_t erase_error, commit_error;
static uint8_t saved_blob[sizeof(passport_preferences_t)];
static bool reload_blob;
static uint8_t ble_flag;
static bool ble_exists;
static bool album_exists;
static photo_album_t album_blob;
static passport_profile_t legacy_profile;
static uint8_t upload[PHOTO_BYTES];
static size_t upload_at, upload_length;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &flash; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) { return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { return 1; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    if (!strcmp(name, "passport_v1")) *out = 1;
    else if (!strcmp(name, "passport_ops")) *out = 2;
    else if (!strcmp(name, "nimble_bond")) *out = 3;
    else abort(); // 原厂身份等其他命名空间必须保持完整。
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    if (!strcmp(key, album_key) && album_exists) {
        assert(*size >= sizeof album_blob);
        memcpy(out, &album_blob, sizeof album_blob);
        *size = sizeof album_blob;
        return ESP_OK;
    }
    if (!strcmp(key, "profile") && legacy_profile.photo) {
        assert(*size >= sizeof legacy_profile);
        memcpy(out, &legacy_profile, sizeof legacy_profile);
        *size = sizeof legacy_profile;
        return ESP_OK;
    }
    if (strcmp(key,"preferences")) return ESP_ERR_NVS_NOT_FOUND;
    if (reload_blob) {
        assert(*size >= sizeof saved_blob);
        memcpy(out, saved_blob, sizeof saved_blob);
        *size = sizeof saved_blob;
        return ESP_OK;
    }
    // 真实旧版布局八字节：升级只能追加休眠计时，不得丢弃已有偏好。
    const uint8_t legacy[8] = {0,70,40,0,60,0,0,0};
    assert(*size>=sizeof legacy); memcpy(out,legacy,sizeof legacy); *size=sizeof legacy; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1);
    if (!strcmp(key, album_key)) {
        assert(size == sizeof album_blob);
        if (!commit_error) {
            memcpy(&album_blob, data, size);
            album_exists = true;
        }
    } else if (!strcmp(key, "profile")) {
        assert(size == sizeof legacy_profile);
        if (!commit_error) memcpy(&legacy_profile, data, size);
    } else {
        assert(!strcmp(key, "preferences") && size == sizeof saved_blob);
        memcpy(saved_blob, data, size);
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { return commit_error; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{ if (handle==1 && !strcmp(key,"ble_off")) { if(!ble_exists)return ESP_ERR_NVS_NOT_FOUND; *out=ble_flag;return ESP_OK; }
  assert(handle == 2 && !strcmp(key, "pending")); if (!pending_exists) return ESP_ERR_NVS_NOT_FOUND; *out = pending_action; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ if(handle==1 && !strcmp(key,"ble_off")) { ble_exists=true;ble_flag=value;return ESP_OK; }
  assert(handle == 2 && !strcmp(key, "pending")); pending_exists = true; pending_action = value; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ assert(handle == 2 && !strcmp(key, "pending")); pending_exists = false; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t handle)
{ if (handle == 1) ++cleared_user; else if (handle == 3) ++cleared_bonds; else abort(); return ESP_OK; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{ assert(!strcmp(name, "assets")); return &flash_partition; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t length)
{ assert(p == &flash_partition && offset + length <= sizeof flash); if (erase_error) return erase_error; memset(flash + offset, 0xff, length); ++erased_assets; return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p, size_t offset, const void *data, size_t size)
{
    assert(p == &flash_partition && offset + size <= sizeof flash);
    memcpy(flash + offset, data, size);
    return ESP_OK;
}
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t offset, size_t size, int type, const void **out, esp_partition_mmap_handle_t *handle)
{ *out = flash; return ESP_OK; }
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return ~crc;
}
int64_t esp_timer_get_time(void) { return 0; }
int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
    if (upload_at == upload_length) return -1;
    if (size > upload_length - upload_at) size = upload_length - upload_at;
    memcpy(out, upload + upload_at, size);
    upload_at += size;
    return (int)size;
}
bool detach_display_asset(const uint8_t *pixels) { return true; }
const char *esp_err_to_name(int err) { return "test error"; }

static void test_photo_album(void)
{
    legacy_profile.photo = 2;
    legacy_profile.badge = 1;
    memset(flash + PHOTO_SLOT_BYTES, 0x25, PHOTO_BYTES);
    memset(flash + 0x50000, 0x75, BADGE_BYTES);
    assert(storage_init() == ESP_OK);
    assert(photo_album_snapshot().slots[0] == 2 && !album_exists);
    assert(*asset_data(true, 2) == 0x25);
    assert(select_next_photo() == ESP_ERR_NOT_FOUND && !album_exists);

    memset(upload, 0x82, sizeof upload);
    uint32_t crc = esp_rom_crc32_le(0, upload, sizeof upload);
    httpd_req_t req = {.content_len = PHOTO_BYTES};
    upload_length = PHOTO_BYTES;
    assert(receive_photo(&req, 2, crc) == ESP_OK);
    assert(select_next_photo() == ESP_OK);
    assert(storage_init() == ESP_OK && photo_album_snapshot().selected == 2);

    memset(upload, 0x49, sizeof upload);
    crc = esp_rom_crc32_le(0, upload, sizeof upload);
    upload_at = 0;
    assert(receive_photo(&req, 1, crc) == ESP_OK);
    photo_album_t before = photo_album_snapshot();
    assert(before.slots[0] == 2 && before.slots[1] != before.slots[2]);
    assert(*asset_data(true, before.slots[1]) == 0x49 && *asset_data(true, before.slots[2]) == 0x82);

    // 传输中断、CRC 失败和目录提交失败都保留当前目录和旧照片。
    upload_at = 0;
    upload_length = 1024;
    assert(receive_photo(&req, 0, crc) == ESP_ERR_TIMEOUT);
    upload_at = 0;
    upload_length = PHOTO_BYTES;
    assert(receive_photo(&req, 0, crc ^ 1) == ESP_ERR_INVALID_CRC);
    upload_at = 0;
    commit_error = ESP_FAIL;
    assert(receive_photo(&req, 0, crc) == ESP_FAIL);
    commit_error = ESP_OK;
    assert(storage_init() == ESP_OK);
    photo_album_t after = photo_album_snapshot();
    assert(!memcmp(&before, &after, sizeof before) && *asset_data(true, 2) == 0x25);

    upload_at = 0;
    assert(receive_photo(&req, 0, crc) == ESP_OK);
    after = photo_album_snapshot();
    assert(after.slots[0] == 4 && after.selected == 2 && *asset_data(true, 4) == 0x49);
    assert(*asset_data(false, 1) == 0x75);
    assert(select_photo(1) == ESP_OK && storage_init() == ESP_OK && photo_album_snapshot().selected == 1);
    assert(select_photo(2) == ESP_OK && photo_album_snapshot().selected == 2);
    assert(remove_photo(2) == ESP_OK && photo_album_snapshot().selected == 0);
    assert(remove_photo(0) == ESP_OK && photo_album_snapshot().selected == 1);
    assert(remove_photo(1) == ESP_OK && storage_init() == ESP_OK);
    assert(!photo_album_snapshot().slots[0] && !photo_album_snapshot().slots[1] && !photo_album_snapshot().slots[2]);
    assert(select_photo(0) == ESP_ERR_INVALID_ARG);
    commit_error = ESP_FAIL;
    assert(remove_badge() == ESP_FAIL && profile_snapshot().badge == 1 && *asset_data(false, 1) == 0x75);
    commit_error = ESP_OK;
    assert(remove_badge() == ESP_OK && storage_init() == ESP_OK && !profile_snapshot().badge);
    assert(profile_snapshot().photo == 2 && *asset_data(false, 1) == 0xff);
}

int main(void)
{
    memset(flash, 0xff, sizeof flash);
    assert(storage_init() == ESP_OK);
    assert(preferences_snapshot().deep_sleep_seconds==300 && preferences_snapshot().sleep_seconds==60);
    bool ble_on=false;
    assert(load_ble_enabled(&ble_on)==ESP_OK && ble_on);
    assert(save_ble_enabled(false)==ESP_OK && load_ble_enabled(&ble_on)==ESP_OK && !ble_on);
    assert(save_ble_enabled(true)==ESP_OK && load_ble_enabled(&ble_on)==ESP_OK && ble_on);
    ble_flag=2; assert(load_ble_enabled(&ble_on)==ESP_ERR_INVALID_ARG); ble_flag=0;
    passport_preferences_t device = {.brightness = 80, .volume = 70, .sleep_seconds = 180, .power_mode = POWER_PORTABLE, .deep_sleep_seconds=900};
    passport_preferences_t stale_page = preferences_snapshot();
    stale_page.page = 2;
    assert(update_preferences(PREF_DEVICE, &device) == ESP_OK);
    assert(update_preferences(PREF_PAGE, &stale_page) == ESP_OK);
    passport_preferences_t state = preferences_snapshot();
    assert(state.page == 2 && state.volume == 70 && state.brightness == 80 && state.power_mode == POWER_PORTABLE);
    assert(state.deep_sleep_seconds==900 && state.sleep_seconds==180);
    // 老固件读到的是合法的 Codex 页，新固件重启则记得小城。
    state.page = CITY;
    assert(update_preferences(PREF_PAGE, &state) == ESP_OK);
    assert(saved_blob[0] == QUOTA);
    reload_blob = true;
    assert(storage_init() == ESP_OK && preferences_snapshot().page == CITY);
    assert(preferences_snapshot().scene == PIXEL_STREET);
    assert(!preferences_snapshot().quota_focus);
    // 场景、展示页和网页设备设置各自合并，其他字段沿用最新已提交值。
    state.scene = PIXEL_HARBOR;
    assert(update_preferences(PREF_SCENE, &state) == ESP_OK);
    assert(update_preferences(PREF_DEVICE, &device) == ESP_OK);
    assert(storage_init() == ESP_OK);
    assert(preferences_snapshot().page == CITY && preferences_snapshot().scene == PIXEL_HARBOR);
    state.page = SETTINGS;
    state.return_page = CITY;
    state.scene = PIXEL_STREET; // 模拟只带旧场景值的页面更新。
    assert(update_preferences(PREF_PAGE, &state) == ESP_OK);
    assert(saved_blob[0] == SETTINGS && saved_blob[3] == QUOTA);
    assert(storage_init() == ESP_OK && preferences_snapshot().return_page == CITY);
    assert(preferences_snapshot().scene == PIXEL_HARBOR);
    commit_error = ESP_FAIL;
    assert(update_preferences(PREF_SCENE, &state) == ESP_FAIL);
    assert(preferences_snapshot().scene == PIXEL_HARBOR);
    commit_error = ESP_OK;
    state.scene = PIXEL_SCENE_COUNT;
    assert(update_preferences(PREF_SCENE, &state) == ESP_ERR_INVALID_ARG);
    state = preferences_snapshot();
    state.page = QUOTA;
    assert(update_preferences(PREF_PAGE, &state) == ESP_OK);
    state.quota_focus = true;
    assert(update_preferences(PREF_QUOTA_VIEW, &state) == ESP_OK);
    assert(storage_init() == ESP_OK);
    assert(preferences_snapshot().page == QUOTA && preferences_snapshot().quota_focus);
    state = preferences_snapshot();
    state.scene = PIXEL_PARK;
    state.quota_focus = false; // 带旧额度布局值的场景更新。
    assert(update_preferences(PREF_SCENE, &state) == ESP_OK);
    assert(storage_init() == ESP_OK);
    assert(preferences_snapshot().scene == PIXEL_PARK && preferences_snapshot().quota_focus);
    assert(update_preferences(PREF_QUOTA_VIEW, &state) == ESP_OK);
    assert(storage_init() == ESP_OK && !preferences_snapshot().quota_focus);
    state.return_page = SETTINGS;
    assert(update_preferences(PREF_PAGE, &state) == ESP_ERR_INVALID_ARG);
    device.deep_sleep_seconds=301;
    assert(update_preferences(PREF_DEEP_SLEEP,&device)==ESP_ERR_INVALID_ARG);
    device.deep_sleep_seconds=900;
    device.sleep_seconds = 7;
    assert(update_preferences(PREF_DEVICE, &device) == ESP_ERR_INVALID_ARG);
    device.sleep_seconds = 300; device.volume = 30;
    commit_error = ESP_FAIL;
    assert(update_preferences(PREF_DEVICE, &device) == ESP_FAIL);
    assert(preferences_snapshot().volume == 70);
    commit_error = ESP_OK;
    assert(schedule_maintenance(MAINTENANCE_FORGET_MAC) == ESP_OK);
    assert(resume_maintenance() == ESP_OK && !pending_exists);
    assert(cleared_bonds == 1 && !cleared_user && !erased_assets);
    assert(schedule_maintenance(MAINTENANCE_RESET) == ESP_OK);
    erase_error = ESP_FAIL;
    assert(resume_maintenance() == ESP_FAIL && pending_exists && !cleared_user);
    erase_error = ESP_OK;
    flash_partition.address = 0x356000;
    assert(resume_maintenance() == ESP_ERR_INVALID_SIZE && pending_exists && !erased_assets);
    flash_partition.address = 0x500000;
    memset(flash, 0x42, sizeof flash);
    assert(resume_maintenance() == ESP_OK && !pending_exists);
    assert(erased_assets == 1 && cleared_user == 1 && cleared_bonds == 2);
    for (size_t i = 0; i < sizeof flash; ++i) assert(flash[i] == 0xff);
    assert(resume_maintenance() == ESP_OK && erased_assets == 1);
    assert(schedule_maintenance(9) == ESP_ERR_INVALID_ARG);
    test_photo_album();
    puts("Storage tests passed: preferences, reset and photo album migration, cycling and safe replacement.");
}
