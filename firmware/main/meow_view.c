#include "meow_view.h"

typedef struct {
    lv_layer_t *layer;
    lv_area_t area;
} painter_t;

static void rect(painter_t *p, int x, int y, int w, int h, uint32_t color)
{
    lv_area_t area = {p->area.x1 + x, p->area.y1 + y, p->area.x1 + x + w - 1, p->area.y1 + y + h - 1};
    if (area.x1 < p->area.x1) area.x1 = p->area.x1;
    if (area.y1 < p->area.y1) area.y1 = p->area.y1;
    if (area.x2 > p->area.x2) area.x2 = p->area.x2;
    if (area.y2 > p->area.y2) area.y2 = p->area.y2;
    if (area.x1 > area.x2 || area.y1 > area.y2) return;
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = lv_color_hex(color);
    lv_draw_fill(p->layer, &fill, &area);
}

static void fish(painter_t *p, int x, int y)
{
    rect(p, x, y + 2, 10, 4, 0xffc875);
    rect(p, x + 2, y, 6, 8, 0xffc875);
    rect(p, x + 9, y, 3, 8, 0xf68f52);
    rect(p, x + 2, y + 2, 2, 2, 0x643559);
}

static void cat(painter_t *p, int x, int y, const meow_game_t *game)
{
    enum { ORANGE = 0xffac52, GOLD = 0xffc06e, SHADOW = 0xe57a46, INK = 0x40263f, CREAM = 0xffe3a8 };
    int stretch = 0;
    if (game->motion == MEOW_REACH) stretch = game->age < 100 ? game->age / 5 : 20;
    if (game->motion == MEOW_PULL) stretch = 20 - game->age / 9;
    int back = x - 16 - stretch;
    int head = x - 1;
    int legs = game->motion == MEOW_RUN && (game->x / (4 * MEOW_UNIT)) % 2 ? 3 : 0;

    rect(p, back - 7, y - 16, 4, 8, SHADOW); // 像素尾巴与快递包。
    rect(p, back - 5, y - 10, 7, 4, ORANGE);
    rect(p, back, y - 15, 19 + stretch, 12, SHADOW);
    rect(p, back, y - 15, 20 + stretch, 8, ORANGE);
    rect(p, back + 2, y - 20, 9, 11, 0x914764);
    rect(p, back + 4, y - 18, 5, 6, 0xe57874);
    rect(p, back + 5 + legs, y - 5, 6, 5, GOLD);
    rect(p, x + 4 - legs, y - 5, 6, 5, ORANGE);
    if (stretch || game->motion == MEOW_FALL) rect(p, x + 9, y - 6, 8, 4, GOLD);
    rect(p, head, y - 24, 16, 18, GOLD);
    rect(p, head, y - 30, 5, 9, ORANGE);
    rect(p, head + 11, y - 30, 5, 9, ORANGE);
    rect(p, head + 4, y - 23, 2, 4, SHADOW);
    rect(p, head + 5, y - 15, 13, 7, CREAM);
    rect(p, head + 10, y - 19, 2, 3, INK);
    rect(p, head + 16, y - 15, 2, 2, SHADOW);
}

static void rooftop(painter_t *p, int left, int right, int ground, int64_t camera)
{
    if (left < 0) left = 0;
    if (right > 240) right = 240;
    if (left >= right) return;
    rect(p, left, ground, right - left, 70, 0x4a3556);
    rect(p, left, ground, right - left, 6, 0xd47d82);
    rect(p, left, ground + 6, right - left, 8, 0x824c68);
    for (int x = left + (16 - (left + camera) % 16) % 16; x < right - 1; x += 16)
        rect(p, x, ground + 16, 2, 7, 0x754b67);
}

// 限制转换为屏幕坐标时的数值范围，长时间跑动也不会让绘制坐标溢出。
static int viewport_x(int64_t world, int64_t camera)
{
    int64_t x = world - camera;
    if (x < -300) return -300;
    if (x > 540) return 540;
    return x;
}

static void draw_awning(painter_t *p, const meow_obstacle_t *o, int x, int ground)
{
    int left = x + MEOW_AWNING_LEFT;
    int width = MEOW_AWNING_RIGHT - MEOW_AWNING_LEFT;
    rect(p, left + 6, ground + 8, 3, 37, 0x9c737e);
    rect(p, left + width - 9, ground + 8, 3, 37, 0x9c737e);
    rect(p, left, ground, width, 8, 0xe88775);
    for (int stripe = 0; stripe < width; stripe += 12)
        rect(p, left + stripe, ground, 6, 8, 0xffcc92);
    rect(p, x - 30, ground + 1, 9, 3, 0xffe6a4);
    rect(p, x + MEOW_AWNING_RIGHT - 12, ground + 1, 7, 3, 0xffe6a4);
    if (!o->fish_taken) {
        fish(p, left + 6, ground - 11);
        fish(p, left + 20, ground - 11);
    }
}

static void draw_cart(painter_t *p, int rail, int x, int ground)
{
    rect(p, rail - MEOW_CART_SWING, ground + 4, MEOW_CART_SWING * 2 + MEOW_CART_WIDTH, 2, 0x9c737e);
    rect(p, x, ground - 15, MEOW_CART_WIDTH, 12, 0x8dbead);
    rect(p, x + 2, ground - 15, MEOW_CART_WIDTH - 4, 3, 0xc1e3b9);
    rect(p, x + 3, ground - 4, 4, 4, 0x322244);
    rect(p, x + MEOW_CART_WIDTH - 6, ground - 4, 4, 4, 0x322244);
    rect(p, x - 30, ground + 1, 9, 3, 0xb5ddd0);
}

static void draw_delivery(painter_t *p, int x, int ground)
{
    rect(p, x - 16, ground - 42, 42, 42, 0x643950);
    rect(p, x - 20, ground - 42, 50, 8, 0xf18a73);
    for (int stripe = x - 16; stripe < x + 28; stripe += 12)
        rect(p, stripe, ground - 42, 6, 8, 0xffc080);
    rect(p, x - 4, ground - 22, 14, 14, 0xf4ba7a);
    rect(p, x + 1, ground - 22, 4, 14, 0xad6f59);
}

static void draw_scene(lv_event_t *event)
{
    const meow_game_t *game = lv_event_get_user_data(event);
    painter_t p = {.layer = lv_event_get_layer(event)};
    lv_obj_get_coords(lv_event_get_target_obj(event), &p.area);
    int ground = lv_area_get_height(&p.area) - 46;
    int64_t camera = game->x / MEOW_UNIT - 53;
    if (lv_area_get_width(&p.area) < 240) camera -= 22;

    rect(&p, 0, 0, 240, ground + 46, 0x211b39);
    rect(&p, 0, ground / 2, 240, ground + 46, 0x322244);
    rect(&p, 180, 22, 18, 26, 0xe6a887);
    rect(&p, 176, 26, 26, 18, 0xe6a887);
    rect(&p, 192, 18, 14, 18, 0x211b39);
    static const uint8_t stars[][2] = {{17,24},{32,51},{64,16},{102,36},{121,13},{141,58},{218,53}};
    for (size_t i = 0; i < sizeof stars / sizeof *stars; ++i)
        rect(&p, stars[i][0], stars[i][1], 2, 2, 0xc793ae);
    for (int i = 0; i < 7; ++i) {
        int scroll = camera > 0 ? camera / 4 % (42 * 40) : 0;
        int x = i * 42 - scroll % 42, building = i + scroll / 42;
        int top = ground - 30 - (building * 19 % 40);
        rect(&p, x, top, 38, 130, building % 2 ? 0x46304e : 0x3b2a44);
        rect(&p, x + 7, top + 9, 4, 8, building % 2 ? 0xa56470 : 0x805567);
        rect(&p, x + 19, top + 21, 4, 6, 0x624254);
    }

    int left = 0;
    for (unsigned i = 0; i < MEOW_TRACK_SIZE; ++i) {
        const meow_obstacle_t *o = &game->track[i];
        if (o->kind != MEOW_GAP && o->kind != MEOW_AWNING) continue;
        int gap_start = viewport_x(o->start, camera);
        rooftop(&p, left, gap_start, ground, camera);
        left = viewport_x(meow_obstacle_end(o), camera);
    }
    rooftop(&p, left, 240, ground, camera);
    for (unsigned i = 0; i < MEOW_TRACK_SIZE; ++i) {
        const meow_obstacle_t *o = &game->track[i];
        int x = viewport_x(o->start, camera);
        if (x < -110 || x > 290) continue;
        switch (o->kind) {
        case MEOW_BOX: {
            int width = meow_obstacle_end(o) - o->start;
            int y = ground - MEOW_BOX_HEIGHT;
            rect(&p, x, y, width, MEOW_BOX_HEIGHT, 0xb66e60);
            rect(&p, x, y, width, 3, 0xe6a077);
            rect(&p, x + 4, y + 6, 10, 2, 0x744150);
            if (!o->fish_taken) fish(&p, x + width + MEOW_BOX_FISH - 6, ground - 12);
            rect(&p, x - 30, ground + 1, 9, 3, 0xffe6a4);
            break;
        }
        case MEOW_GAP:
            rect(&p, x - 30, ground + 1, 9, 3, 0x8de2d3);
            break;
        case MEOW_AWNING:
            draw_awning(&p, o, x, ground);
            break;
        case MEOW_CART:
            draw_cart(&p, x, (int)(meow_cart_position(o, game->elapsed_ms) - camera), ground);
            break;
        case MEOW_DELIVERY:
            draw_delivery(&p, x, ground);
            break;
        case MEOW_NONE:
            break;
        }
    }

    cat(&p, (int)(game->x / MEOW_UNIT - camera), ground - game->height, game);
}

void meow_view_bind(lv_obj_t *viewport, const meow_game_t *snapshot)
{
    lv_obj_add_event_cb(viewport, draw_scene, LV_EVENT_DRAW_MAIN, (void *)snapshot);
}
