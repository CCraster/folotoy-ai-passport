#include "passport.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"

esp_err_t storage_usage(storage_usage_t *usage)
{
    // 固件元数据仅首次读取，资料占用只统计已提交的槽位。
    static storage_usage_t base;
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&lock);
    *usage = base;
    portEXIT_CRITICAL(&lock);
    if (!usage->total) {
        storage_usage_t next = {0};
        const esp_partition_t *app = esp_ota_get_running_partition();
        const esp_partition_t *assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
        if (!app || !assets) return ESP_ERR_NOT_FOUND;
        esp_partition_pos_t position = {.offset = app->address, .size = app->size};
        esp_image_metadata_t metadata = {0};
        esp_err_t err = esp_image_get_metadata(&position, &metadata);
        if (err != ESP_OK) return err;
        err = esp_flash_get_size(NULL, &next.total);
        if (err != ESP_OK) return err;
        next.firmware = metadata.image_len;
        next.firmware_capacity = app->size;
        next.assets_capacity = assets->size;
        portENTER_CRITICAL(&lock);
        base = next;
        portEXIT_CRITICAL(&lock);
        *usage = next;
    }
    passport_profile_t content = profile_snapshot();
    usage->badge = content.badge ? (content.portrait ? BADGE_BYTES : LEGACY_BADGE_BYTES) : 0;
    photo_album_t album = photo_album_snapshot();
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i)
        usage->photo += album.bytes[i];
    return ESP_OK;
}
