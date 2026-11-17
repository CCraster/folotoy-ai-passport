#include "pixel_city.h"
#include "passport.h"

#include <string.h>
#include "src/draw/lv_image_decoder_private.h"

extern const uint8_t art[] asm("_binary_pixel_city_rle_start");
enum { LIGHTS = 3, BAND_ROWS = 4 };
static lv_image_dsc_t backgrounds[PIXEL_SCENE_COUNT][LIGHTS];
static lv_draw_buf_t band;
static _Alignas(16) uint16_t band_pixels[WIDTH * BAND_ROWS];
static const uint8_t *band_source;
static int band_y;

static uint16_t read_le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t art_offset(unsigned index)
{
    const uint8_t *p = art + index * 4;
    return read_le16(p) | (uint32_t)read_le16(p + 2) << 16;
}

static lv_result_t scene_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                              lv_image_header_t *header)
{
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    for (unsigned scene = 0; scene < PIXEL_SCENE_COUNT; ++scene) {
        for (unsigned light = 0; light < LIGHTS; ++light) {
            const lv_image_dsc_t *image = &backgrounds[scene][light];
            if (dsc->src != image) continue;
            *header = image->header;
            header->cf = LV_COLOR_FORMAT_RGB565;
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t scene_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    (void)dsc;
    return LV_RESULT_OK;
}

static void decode_row(const uint8_t *image, unsigned y, uint16_t *out)
{
    // The generator validates all rows before embedding the atlas in flash.
    const uint8_t *run = image + read_le16(image + y * 2);
    if (!*run) {
        memcpy(out, run + 1, WIDTH * sizeof(*out));
        return;
    }
    uint16_t *end = out + WIDTH;
    while (out < end) {
        unsigned count = *run++;
        uint16_t color = read_le16(run);
        run += 2;
        while (count--) *out++ = color;
    }
}

static lv_result_t scene_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                              const lv_area_t *requested, lv_area_t *decoded)
{
    (void)decoder;
    const lv_image_dsc_t *image = dsc->src;
    int y = decoded->y1 == LV_COORD_MIN ? requested->y1 : decoded->y2 + 1;
    if (y < 0 || y >= HEIGHT || y > requested->y2) return LV_RESULT_INVALID;
    int first = y / BAND_ROWS * BAND_ROWS;
    if (band_source != image->data || band_y != first) {
        for (int row = 0; row < BAND_ROWS; ++row)
            decode_row(image->data, first + row, band_pixels + row * WIDTH);
        band_source = image->data;
        band_y = first;
    }
    *decoded = (lv_area_t){0, first, WIDTH - 1, first + BAND_ROWS - 1};
    dsc->decoded = &band;
    return LV_RESULT_OK;
}

bool pixel_city_install_decoder(void)
{
    _Static_assert(HEIGHT % BAND_ROWS == 0, "Bands must fit the screen");
    if (lv_draw_buf_init(&band, WIDTH, BAND_ROWS, LV_COLOR_FORMAT_RGB565,
                         WIDTH * 2, band_pixels, sizeof band_pixels) != LV_RESULT_OK) return false;
    for (unsigned scene = 0; scene < PIXEL_SCENE_COUNT; ++scene) {
        for (unsigned light = 0; light < LIGHTS; ++light) {
            unsigned index = scene * LIGHTS + light;
            uint32_t start = art_offset(index);
            backgrounds[scene][light] = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA,
                           .w = WIDTH, .h = HEIGHT, .stride = WIDTH * 2},
                .data = art + start, .data_size = art_offset(index + 1) - start,
            };
        }
    }
    lv_image_decoder_t *decoder = lv_image_decoder_create();
    if (!decoder) return false;
    decoder->name = "Passport pixel scenes";
    lv_image_decoder_set_info_cb(decoder, scene_info);
    lv_image_decoder_set_open_cb(decoder, scene_open);
    lv_image_decoder_set_get_area_cb(decoder, scene_area);
    return true;
}

typedef struct { int x, y, w, h; uint32_t color; } pixel_rect_t;

enum { BUS, WALKER_GREEN, WALKER_TAN, CAT, DOG, BIKE, TAXI, BIRD, BUTTERFLY, SAILBOAT, WAVE, WINDOW, DUCK };
static const pixel_rect_t bus[] = {
    {0,15,45,2,0x1c3036},{2,1,37,15,0xbecbb6},{0,5,43,10,0xbecbb6},
    {2,10,39,4,0x3c9b72},{4,3,25,6,0x233b46},
    {6,4,4,4,0x90b6a1},{12,4,4,4,0x90b6a1},{18,4,4,4,0x90b6a1},{24,4,4,4,0x90b6a1},
    {31,3,8,7,0x284750},{33,4,5,4,0x78aaa0},{28,3,1,11,0x637f77},
    {41,9,2,3,0xfff2c4},{0,10,2,2,0xb97865},{7,13,7,5,0x142931},
    {31,13,7,5,0x142931},{9,15,3,2,0x7b9891},{33,15,3,2,0x7b9891},{10,0,12,1,0xe6dcc0},
};
static const pixel_rect_t person[] = {
    {0,14,8,1,0x1c3036},{2,1,4,4,0xd6b99b},{1,0,5,2,0x25353d},
    {2,5,4,5,0x91b6a2},{1,6,1,3,0xd6b99b},
};
static const pixel_rect_t cat[] = {
    {3,5,9,4,0xe7d3a7},{10,2,5,5,0xe7d3a7},{10,0,2,3,0xe7d3a7},{14,0,1,3,0xe7d3a7},
    {13,4,1,1,0x283536},{0,3,2,5,0xe7d3a7},{1,5,3,2,0xe7d3a7},
};

static const pixel_rect_t dog[] = {
    {2,4,10,4,0xb77d54},{11,2,5,5,0xc99465},{12,1,2,4,0x704a41},
    {15,4,2,2,0xe4c7a4},{15,3,1,1,0x25252b},{0,2,3,2,0xb77d54},
};
static const pixel_rect_t bike[] = {
    {1,9,4,1,0x96b9ba},{0,10,6,4,0x273440},{1,14,4,1,0x96b9ba},
    {13,9,4,1,0x96b9ba},{12,10,6,4,0x273440},{13,14,4,1,0x96b9ba},
    {4,11,10,2,0x58b5a3},{9,7,5,2,0xf7c682},{8,3,4,5,0xd9887f},
    {9,0,4,3,0xf5d6ab},{12,6,4,1,0xeff3ca},{7,8,2,3,0x335057},
};
static const pixel_rect_t taxi[] = {
    {0,12,27,2,0x24343c},{0,6,27,6,0xd9ae63},{5,2,15,7,0xd9ae63},
    {7,3,10,3,0x374e58},{12,3,1,4,0xd9ae63},{11,0,5,2,0xf5df9d},
    {4,10,5,4,0x172b37},{19,10,5,4,0x172b37},{0,7,2,2,0xffedba},
};
static const pixel_rect_t sailboat[] = {
    {0,25,35,2,0x639298},{5,21,26,4,0x733f43},{3,19,31,3,0xd68172},
    {18,0,2,20,0xe4d1ab},{15,4,3,13,0xe8e4ca},{12,8,3,9,0xe8e4ca},
    {9,12,3,5,0xe8e4ca},{6,15,3,2,0xe8e4ca},{21,6,2,11,0xb6d6cb},
    {23,10,3,7,0xb6d6cb},{26,13,4,4,0xb6d6cb},
};
static const pixel_rect_t wave[] = {{0,0,10,1,0x6e9b9f},{4,2,15,1,0x507f90}};
static const pixel_rect_t window_light[] = {{0,0,8,11,0xb89b6b},{0,0,3,2,0xebd4a5},{4,0,1,11,0x766750}};
static const pixel_rect_t duck[] = {{0,7,15,1,0x719596},{2,4,9,3,0xe8d3a1},
                                   {9,2,4,3,0xe8d3a1},{12,3,2,1,0xe0a877}};
#define SHAPE(name) {name, sizeof(name) / sizeof(*(name))}
typedef struct { const pixel_rect_t *pixels; size_t count; } sprite_shape_t;
static const sprite_shape_t shapes[] = {
    [BUS] = SHAPE(bus), [WALKER_GREEN] = SHAPE(person), [WALKER_TAN] = SHAPE(person),
    [CAT] = SHAPE(cat), [DOG] = SHAPE(dog), [BIKE] = SHAPE(bike), [TAXI] = SHAPE(taxi),
    [BIRD] = {NULL, 0}, [BUTTERFLY] = {NULL, 0}, [SAILBOAT] = SHAPE(sailboat),
    [WAVE] = SHAPE(wave), [WINDOW] = SHAPE(window_light), [DUCK] = SHAPE(duck),
};

static void draw_pixel(lv_layer_t *layer, const lv_area_t *origin, pixel_rect_t px, bool mirror)
{
    int x = origin->x1 + (mirror ? origin->x2 - origin->x1 + 1 - px.x - px.w : px.x);
    int y = origin->y1 + px.y;
    lv_area_t rect = {x, y, x + px.w - 1, y + px.h - 1};
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = lv_color_hex(px.color);
    lv_draw_fill(layer, &fill, &rect);
}

static void draw_sprite(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    uintptr_t kind = (uintptr_t)lv_event_get_user_data(event);
    const sprite_shape_t shape = shapes[kind];
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);
    bool mirror = kind == WALKER_TAN;
    bool step = (uintptr_t)lv_obj_get_user_data(obj) != 0;
    for (size_t i = 0; i < shape.count; ++i) {
        pixel_rect_t px = shape.pixels[i];
        if (kind == WALKER_TAN && i == 3) px.color = 0xbd9c7d;
        draw_pixel(layer, &origin, px, mirror);
    }
    if (kind == CAT) {
        draw_pixel(layer, &origin, (pixel_rect_t){4,9,2,step ? 2 : 1,0xe7d3a7}, false);
        draw_pixel(layer, &origin, (pixel_rect_t){10,9,2,step ? 1 : 2,0xe7d3a7}, false);
    } else if (kind == DOG) {
        draw_pixel(layer, &origin, (pixel_rect_t){step ? 4 : 5,7,2,3,0xb77d54}, false);
        draw_pixel(layer, &origin, (pixel_rect_t){step ? 10 : 9,7,2,3,0xb77d54}, false);
    } else if (kind == BIRD || kind == BUTTERFLY) {
        uint32_t ink = kind == BIRD ? 0xc1cfd1 : 0xf0b183;
        draw_pixel(layer, &origin, (pixel_rect_t){0,step ? 0 : 3,3,1,ink}, false);
        draw_pixel(layer, &origin, (pixel_rect_t){3,2,3,1,ink}, false);
        draw_pixel(layer, &origin, (pixel_rect_t){6,step ? 0 : 3,3,1,ink}, false);
    } else if (kind == BIKE) {
        draw_pixel(layer, &origin, (pixel_rect_t){step ? 8 : 10,12,2,2,0xcfb596}, false);
    } else if (kind == WALKER_GREEN || kind == WALKER_TAN) {
        draw_pixel(layer, &origin, (pixel_rect_t){step ? 1 : 2,10,2,4,0x233742}, mirror);
        draw_pixel(layer, &origin, (pixel_rect_t){step ? 5 : 4,10,2,4,0x233742}, mirror);
    }
}

static lv_obj_t *sprite(lv_obj_t *parent, unsigned kind, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(obj, width, height);
    lv_obj_add_event_cb(obj, draw_sprite, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)kind);
    return obj;
}

typedef struct { unsigned kind; int width, height; } actor_t;
static const actor_t actors[PIXEL_SCENE_COUNT][8] = {
    [PIXEL_STREET] = {{WALKER_GREEN,8,15},{WALKER_TAN,8,15},{CAT,15,11},{BUS,45,18},
                      {DOG,17,10},{BIKE,18,15},{TAXI,27,14},{WINDOW,8,11}},
    [PIXEL_PARK] = {{WALKER_GREEN,8,15},{DOG,17,10},{BIKE,18,15},{BUTTERFLY,9,4},
                    {BUTTERFLY,9,4},{DUCK,15,8},{DUCK,15,8}},
    [PIXEL_HARBOR] = {{SAILBOAT,35,27},{BIRD,9,4},{BIRD,9,4},{WAVE,19,3},
                      {WAVE,19,3},{WALKER_TAN,8,15},{WINDOW,8,11}},
};

void pixel_city_init(pixel_city_t *city, lv_obj_t *parent, pixel_scene_t scene)
{
    *city = (pixel_city_t){.scene = scene, .lighting = -1};
    city->background = lv_image_create(parent);
    for (size_t i = 0; i < sizeof(city->actors) / sizeof(*city->actors); ++i) {
        const actor_t actor = actors[scene][i];
        if (actor.width) city->actors[i] = sprite(parent, actor.kind, actor.width, actor.height);
    }
}

static void move_sprite(lv_obj_t *obj, int x, int y, bool step)
{
    lv_obj_set_pos(obj, x, y);
    if ((uintptr_t)lv_obj_get_user_data(obj) != step) {
        lv_obj_set_user_data(obj, (void *)(uintptr_t)step);
        lv_obj_invalidate(obj);
    }
}

// Distance in native pixels, calculated from elapsed time before rounding.
static unsigned distance_at(uint32_t ms, unsigned pixels_per_second)
{
    return ms / 1000 * pixels_per_second + ms % 1000 * pixels_per_second / 1000;
}

static void move_street(pixel_city_t *city, uint32_t ms, bool night)
{
    lv_obj_t **a = city->actors;
    bool step = (ms / 240) % 2;
    int walker = (int)((distance_at(ms, 5) + 42) % 280) - 25;
    move_sprite(a[0], walker, 240, step);
    move_sprite(a[1], 249 - (int)((distance_at(ms, 4) + 24) % 268), 237, !step);
    move_sprite(a[4], walker + 13, 245, !step);
    move_sprite(a[5], (int)((distance_at(ms, 14) + 110) % 285) - 20, 253, step);
    move_sprite(a[6], 252 - (int)(distance_at((ms + 12000) % 32000, 30)), 286, false);
    if (night && (ms / 9000) % 3 != 1) lv_obj_remove_flag(a[7], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(a[7], LV_OBJ_FLAG_HIDDEN);
    move_sprite(a[7], 136, 170, false);
    int phase = ms % 40000;
    int x = -15 + phase * 77 / 14000, y = 150;
    if (phase >= 14000 && phase < 17000) {
        int u = phase - 14000;
        x = 62 + u * 24 / 3000;
        y = 150 - u * 35 / 3000 - u * (3000-u) / 160000;
    } else if (phase >= 17000 && phase < 26000) {
        x = 86 + (phase - 17000) * 63 / 9000;
        y = 115;
    } else if (phase >= 26000 && phase < 29000) {
        int u = phase - 26000;
        x = 149 + u * 31 / 3000;
        y = 115 + u * 38 / 3000 - u * (3000-u) / 140000;
    } else if (phase >= 29000) {
        x = 180 + (phase - 29000) * 7 / 1000;
        y = 153;
    }
    move_sprite(a[2], x, y, step);
    move_sprite(a[3], (int)distance_at((ms+3000) % 24000, 22) - 47, 263, false);
}

// Triangle motion keeps the pond and air activity on screen rather than wrapping.
static int swing(unsigned traveled, unsigned distance)
{
    unsigned phase = traveled % (2 * distance);
    return phase < distance ? phase : 2 * distance - phase;
}

static void move_park(lv_obj_t **a, uint32_t ms)
{
    bool step = (ms / 240) % 2;
    int walker = (int)((distance_at(ms, 6) + 60) % 276) - 23;
    move_sprite(a[0], walker, 266, step);
    move_sprite(a[1], walker + 13, 271, !step);
    move_sprite(a[2], (int)((distance_at(ms, 13) + 124) % 289) - 22, 279, step);
    move_sprite(a[3], 31 + swing(distance_at(ms, 6), 73), 211 - swing(distance_at(ms, 10), 18), step);
    move_sprite(a[4], 162 + swing(distance_at(ms, 5) + 45, 65),
                242 - swing(distance_at(ms, 10) + 11, 19), !step);
    const int duck_x = swing(distance_at(ms, 4), 71);
    const int duck_y = swing(distance_at(ms, 3), 9);
    move_sprite(a[5], 103 + duck_x, 216 + duck_y, false);
    move_sprite(a[6], 93 + duck_x, 222 + duck_y, false);
}

static void move_harbor(lv_obj_t **a, uint32_t ms, bool night)
{
    bool step = (ms / 200) % 2;
    move_sprite(a[0], (int)((distance_at(ms, 6) + 88) % 282) - 38,
                191 + swing(distance_at(ms, 3), 3), false);
    move_sprite(a[1], (int)((distance_at(ms, 14) + 36) % 270) - 11,
                93 + swing(distance_at(ms, 6), 12), step);
    move_sprite(a[2], (int)((distance_at(ms, 14) + 21) % 270) - 11,
                104 + swing(distance_at(ms, 6) + 9, 13), !step);
    move_sprite(a[3], 111 + swing(distance_at(ms, 10), 15), 239 + swing(distance_at(ms, 3), 3), false);
    move_sprite(a[4], 197 + swing(distance_at(ms, 10) + 22, 24), 221 + swing(distance_at(ms, 3), 5), false);
    move_sprite(a[5], 249 - (int)((distance_at(ms, 4) + 48) % 265), 278, step);
    move_sprite(a[6], 37, 140, false);
    if (night && (ms % 4800) < 1600) lv_obj_remove_flag(a[6], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(a[6], LV_OBJ_FLAG_HIDDEN);
}

void pixel_city_update(pixel_city_t *city, int hour, uint32_t elapsed_ms)
{
    int lighting = hour >= 6 && hour < 17 ? 0 : hour >= 17 && hour < 19 ? 1 : 2;
    if (lighting != city->lighting) {
        city->lighting = lighting;
        lv_image_set_src(city->background, &backgrounds[city->scene][lighting]);
    }

    switch (city->scene) {
    case PIXEL_STREET: move_street(city, elapsed_ms, lighting != 0); break;
    case PIXEL_PARK: move_park(city->actors, elapsed_ms); break;
    case PIXEL_HARBOR: move_harbor(city->actors, elapsed_ms, lighting != 0); break;
    default: break;
    }
}
