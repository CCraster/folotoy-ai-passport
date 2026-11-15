#pragma once

#include "lvgl.h"
#include "meow_delivery.h"

// The caller owns the snapshot and updates it under the LVGL lock.
void meow_view_bind(lv_obj_t *viewport, const meow_game_t *snapshot);
