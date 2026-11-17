#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t photo_jpeg_validate_upload(const uint8_t *data, size_t size);
size_t photo_jpeg_from_rgb565(const uint8_t *pixels, uint8_t *jpeg);
bool photo_jpeg_install_decoder(void);
void photo_jpeg_finish_frame(void);
