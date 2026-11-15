#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t address; size_t size; } esp_partition_t;
typedef int esp_partition_mmap_handle_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_MMAP_DATA 1
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t length);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *data, size_t length);
esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset, size_t length, int type, const void **out, esp_partition_mmap_handle_t *handle);
