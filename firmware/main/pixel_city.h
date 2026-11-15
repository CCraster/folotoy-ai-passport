#pragma once

#include "lvgl.h"
#include "passport.h"

typedef struct {
    lv_obj_t *background, *actors[8];
    pixel_scene_t scene;
    int lighting;
} pixel_city_t;

void pixel_city_init(pixel_city_t *city, lv_obj_t *parent, pixel_scene_t scene);
void pixel_city_update(pixel_city_t *city, int hour, uint32_t elapsed_ms);
