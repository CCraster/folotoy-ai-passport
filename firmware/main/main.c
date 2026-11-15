#include "passport.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "music.h"
#include "meow_view.h"
#include "network.h"
#include "power.h"
#include "pixel_city.h"
#include "photo_jpeg.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "passport_release.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

LV_FONT_DECLARE(passport_font)
LV_FONT_DECLARE(passport_heading)
LV_FONT_DECLARE(passport_caption)
LV_FONT_DECLARE(passport_quota)
LV_FONT_DECLARE(passport_game)

static const uint8_t display_pages[] = {BADGE, PHOTO, QUOTA, CITY, GAME};
enum { WEB_SETUP, PAIR, BRIGHTNESS, VOLUME, BOOT_MUSIC, SLEEP, POWER_MODE, FORGET_MAC, RESET, WIFI_POWER, BLE_POWER, MUSIC_ENABLED_SETTING, MUSIC_SELECTION_SETTING, DEEP_SLEEP_SETTING, ABOUT, TASK_CHIME, TASK_CHIME_ENABLE, TASK_CHIME_VOLUME };
enum { MUSIC_ENABLE, MUSIC_MELODY, MUSIC_PREVIEW, MUSIC_MENU_COUNT };
enum { CHIME_ENABLE, CHIME_FOLLOW, CHIME_LEVEL, CHIME_PREVIEW, CHIME_MENU_COUNT };
enum { SOUND_NONE, SOUND_BOOT, SOUND_CHIME };
static const uint16_t sleep_options[] = {30, 60, 180, 300, 0};
static const uint16_t deep_sleep_options[] = {300, 900, 1800};
static const char *weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
static QueueHandle_t buttons;
static lv_obj_t *root, *header, *clock_label, *weekday_label, *wifi_icon, *bluetooth_icon;
static lv_obj_t *battery_label, *battery_icon;
static lv_obj_t *body;
static pixel_city_t city;
static lv_obj_t *city_title;
static int64_t city_title_until;
static int64_t photo_counter_until;
static lv_obj_t *photo_counter, *completion_toast;
static int64_t next_city_frame;
enum { CITY_FRAME_US = 40000 };
static struct {
    int64_t render_at;
    unsigned samples, total_us, peak_us;
    bool skip_first;
} city_render;
static lv_obj_t *image_view;
static lv_image_dsc_t image, identity_text;
static bool image_detached;
static uint8_t displayed_badge, displayed_photo;
// selection 只在可操作项之间循环；返回统一由长按确认键触发。
static uint8_t settings_group, selection;
static int settings_scroll_y;
static const uint8_t settings_items[][5] = {
    {WIFI_POWER, WEB_SETUP, PAIR},
    {SLEEP, BRIGHTNESS, VOLUME, BOOT_MUSIC, TASK_CHIME},
    {POWER_MODE, RESET, ABOUT},
};
static const uint8_t settings_counts[] = {3, 5, 3};
static const char *settings_titles[] = {"连接", "显示与声音", "设备"};
// 草稿只在确认保存时提交；进入或长按退出不会更改设备配置。
static int editing = -1, edit_draft, edit_saved;
static bool bluetooth_visible, power_visible, about_visible;
static uint8_t bluetooth_menu, power_menu;
static bool web_visible;
static bool web_join_qr;
static bool pairing_visible;
static uint8_t sound_panel;
static bool storage_visible;
static uint8_t music_menu;
static uint8_t maintenance_action;
static bool confirm_selected;
static passport_preferences_t preferences;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
static bool ignore_wake_key;
static int64_t ignore_keys_until;
static int64_t config_deadline;
static int64_t last_input;
static bool dark;
static int battery = -1;
static bool dirty;
enum { GAME_COVER, GAME_PLAY, GAME_PAUSED, GAME_RESULT, GAME_MIC_ERROR };
enum { GAME_FRAME_US = 33333 };
static uint8_t game_screen;
static uint32_t game_best;
static meow_game_t game, game_paint;
static bool game_ready, game_voiced, game_tutorial_done;
static int64_t last_game_frame, fish_hint_until, delivery_hint_until;
static lv_obj_t *game_view, *game_hint, *game_score, *game_totals;
static const char *notice;

enum {
    BG = 0x0a0b0d, SURFACE = 0x191c21, SELECTED = 0x23382d,
    TEXT = 0xf5f9fe, MUTED = 0xa6aab5, INACTIVE = 0x666c77,
    ACCENT = 0x32f08c, TRACK = 0x353a43, LOW = 0xffb778,
    INK = 0x0a0b0d, DANGER = 0xff6b73,
};

static lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                      const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *item = lv_label_create(parent);
    lv_obj_set_style_text_font(item, font, 0);
    lv_obj_set_style_text_color(item, color(rgb), 0);
    lv_obj_set_pos(item, x, y);
    lv_label_set_text(item, text);
    return item;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t rgb)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_size(item, w, h);
    lv_obj_set_style_bg_color(item, color(rgb), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    return item;
}

static lv_obj_t *rounded_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t rgb, int radius)
{
    lv_obj_t *item = box(parent, x, y, w, h, rgb);
    lv_obj_set_style_radius(item, radius, 0);
    return item;
}

static lv_obj_t *centered(lv_obj_t *parent, const char *text, int y, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *item = label(parent, text, 0, y, font, rgb);
    lv_obj_set_width(item, WIDTH);
    lv_obj_set_style_text_align(item, LV_TEXT_ALIGN_CENTER, 0);
    return item;
}

static void toggle_control(lv_obj_t *parent, int x, int y, bool enabled)
{
    rounded_box(parent, x, y, 36, 20, enabled ? ACCENT : TRACK, 10);
    rounded_box(parent, x + (enabled ? 18 : 3), y + 3, 14, 14, enabled ? INK : TEXT, 7);
}

static lv_obj_t *focus_card(lv_obj_t *parent, int y, int height, bool selected)
{
    lv_obj_t *card = rounded_box(parent, 16, y, 208, height, selected ? SELECTED : SURFACE, 12);
    if (selected) {
        lv_obj_set_style_border_color(card, color(ACCENT), 0);
        lv_obj_set_style_border_width(card, 1, 0);
    }
    return card;
}

static void draw_sun(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t area; lv_obj_get_coords(obj, &area);
    int size = lv_area_get_width(&area), half = (size - 1) / 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
    arc.center = (lv_point_t){area.x1 + half, area.y1 + half};
    arc.radius = size * 3 / 14; arc.width = size > 20 ? 2 : 1;
    arc.start_angle = 0; arc.end_angle = 360;
    arc.color = lv_obj_get_style_text_color(obj, 0);
    lv_draw_arc(layer, &arc);
    static const int8_t rays[8][4] = {{0,-5,0,-7},{4,-4,6,-6},{5,0,7,0},{4,4,6,6},
                                     {0,5,0,7},{-4,4,-6,6},{-5,0,-7,0},{-4,-4,-6,-6}};
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
    line.color = arc.color; line.width = arc.width; line.round_start = line.round_end = 1;
    for (size_t i = 0; i < 8; ++i) {
        line.p1 = (lv_point_precise_t){arc.center.x + rays[i][0] * half / 7, arc.center.y + rays[i][1] * half / 7};
        line.p2 = (lv_point_precise_t){arc.center.x + rays[i][2] * half / 7, arc.center.y + rays[i][3] * half / 7};
        lv_draw_line(layer, &line);
    }
}

static void sun_icon(lv_obj_t *parent, int x, int y, int size, uint32_t ink)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(icon, x, y); lv_obj_set_size(icon, size, size);
    lv_obj_set_style_text_color(icon, color(ink), 0);
    lv_obj_add_event_cb(icon, draw_sun, LV_EVENT_DRAW_MAIN, NULL);
}

static void back_hint(void)
{
    centered(body, editing >= 0 ? "确认保存 · 长按取消" : web_visible ? "上下切换 · 长按确认返回" : "长按确认返回",
             261, &passport_caption, MUTED);
}

static void set_label(lv_obj_t *item, const char *text)
{
    if (strcmp(lv_label_get_text(item), text)) lv_label_set_text(item, text);
}

static void poll_battery(int64_t now)
{
    static int64_t next_read;
    static bool reported;
    static esp_err_t previous_error;
    if (now < next_read) return;

    esp_err_t err = bsp_battery_wake();
    int mv = -1, soc = -1;
    if (err == ESP_OK) {
        mv = bsp_battery_mv();
        soc = bsp_battery_soc();
    }
    int reading = mv >= 2500 ? soc : -1;
    if (reading < 0 && (!reported || battery >= 0 || err != previous_error)) {
        ESP_LOGW("passport", "Battery reading not ready: wake=%s voltage=%d mV SOC=%d",
                 esp_err_to_name(err), mv, soc);
    } else if (reading >= 0 && battery < 0) {
        ESP_LOGI("passport", "Battery reading ready: voltage=%d mV SOC=%d%%", mv, soc);
    }
    reported = true;
    previous_error = err;
    battery = reading;
    next_read = now + (reading < 0 ? 3000000 : 15000000);
}

static void update_header(const web_status_t *web, time_t now)
{
    char time_text[16] = "--:--", power[12] = "--%";
    struct tm clock = local_clock(now);
    if (now >= 1700000000)
        snprintf(time_text, sizeof time_text, "%02d:%02d", clock.tm_hour, clock.tm_min);
    if (battery >= 0) snprintf(power, sizeof power, "%d%%", battery);
    set_label(clock_label, time_text);
    set_label(weekday_label, now >= 1700000000 ? weekdays[clock.tm_wday] : "--");
    static uint32_t wifi_color, bt_color;
    uint32_t network = web->hotspot ? 0xffd795 : web->wifi_connected ? ACCENT : INACTIVE;
    uint32_t bluetooth = quota_connected() ? ACCENT : INACTIVE;
    lv_opa_t bt_opacity = quota_ble_enabled() ? LV_OPA_COVER : LV_OPA_30;
    if (lv_obj_get_style_text_opa(bluetooth_icon, 0) != bt_opacity)
        lv_obj_set_style_text_opa(bluetooth_icon, bt_opacity, 0);
    if (network != wifi_color) {
        wifi_color = network;
        lv_obj_set_style_text_color(wifi_icon, color(network), 0);
    }
    if (bluetooth != bt_color) {
        bt_color = bluetooth;
        lv_obj_set_style_text_color(bluetooth_icon, color(bluetooth), 0);
    }
    if (strcmp(lv_label_get_text(battery_label), power)) {
        static const char *levels[] = {LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
                                      LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL};
        set_label(battery_label, power);
        // 电池图标固定，百分比和连接图标从右向左按文字宽度排列。
        lv_obj_update_layout(header);
        int battery_x = 202 - lv_obj_get_width(battery_label);
        int bluetooth_x = battery_x - lv_obj_get_width(bluetooth_icon) - 6;
        lv_obj_set_x(battery_label, battery_x);
        lv_obj_set_x(bluetooth_icon, bluetooth_x);
        lv_obj_set_x(wifi_icon, bluetooth_x - 23);
        set_label(battery_icon, levels[battery > 0 ? (battery * 4 + 99) / 100 : 0]);
        lv_obj_set_style_text_color(battery_icon, color(battery >= 0 && battery <= 15 ? LOW : TEXT), 0);
    }
}

static void render_image_data(const uint8_t *pixels, int width, int height, size_t size, lv_color_format_t format)
{
    lv_image_cache_drop(&image);
    image = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = format,
                   .w = width, .h = height, .stride = width * 2},
        .data_size = size, .data = pixels,
    };
    image_view = box(body, 0, 0, width, height, BG);
    lv_obj_t *bitmap = lv_image_create(image_view);
    lv_image_set_src(bitmap, &image);
}

// 已保存的身份页包含浏览器栅格化文字。按真实字形上缘截取右栏，
// 让旧数据也能顶对齐；直接引用原 RGB565 行，不缩放、不分配整帧缓冲。
static void align_identity_text(const uint8_t *pixels)
{
    const uint16_t background = ((BG >> 19) << 11) | (((BG >> 10) & 63) << 5) | ((BG >> 3) & 31);
    int top = 16;
    for (; top < 192; ++top) {
        bool ink = false;
        for (int x = 142; x < 224; ++x) {
            size_t offset = (top * WIDTH + x) * 2;
            if ((uint16_t)(pixels[offset] | pixels[offset + 1] << 8) != background) { ink = true; break; }
        }
        if (ink) break;
    }
    if (top == 16 || top == 192) return;
    box(image_view, 142, 16, 82, 176, BG);
    lv_image_cache_drop(&identity_text);
    int height = 192 - top;
    identity_text = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = 82, .h = height, .stride = WIDTH * 2},
        .data_size = height * WIDTH * 2,
        .data = pixels + (top * WIDTH + 142) * 2,
    };
    lv_obj_t *text = lv_image_create(image_view);
    lv_image_set_src(text, &identity_text);
    lv_obj_set_pos(text, 142, 16);
}

bool detach_display_asset(const uint8_t *pixels)
{
    if (!bsp_lvgl_lock(200)) return false;
    // Two uploads can arrive before the UI observes the first commit.
    if (image_view && image.data == pixels) {
        lv_obj_delete(image_view);
        lv_image_cache_drop(&image);
        lv_image_cache_drop(&identity_text);
        image_view = NULL;
        image_detached = true;
    }
    bsp_lvgl_unlock();
    return true;
}

static void identity_icon(lv_obj_t *parent, int x, int y, uint32_t ink)
{
    lv_obj_t *frame = rounded_box(parent, x, y, 22, 18, BG, 3);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, color(ink), 0);
    rounded_box(parent, x + 4, y + 4, 4, 4, ink, 2);
    rounded_box(parent, x + 3, y + 9, 7, 5, ink, 2);
    box(parent, x + 13, y + 5, 5, 1, ink);
    box(parent, x + 13, y + 10, 5, 1, ink);
}

static void person_placeholder(lv_obj_t *parent)
{
    lv_obj_t *head = rounded_box(parent, 55, 65, 34, 34, SURFACE, 17);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, color(ACCENT), 0);
    lv_obj_t *bust = box(parent, 43, 110, 58, 29, SURFACE);
    lv_obj_t *shoulders = rounded_box(bust, 0, 0, 58, 56, SURFACE, 20);
    lv_obj_set_style_border_width(shoulders, 2, 0);
    lv_obj_set_style_border_color(shoulders, color(ACCENT), 0);
    box(bust, 1, 27, 56, 2, ACCENT);
}

static void quota_icon(lv_obj_t *parent, int x, int y, uint32_t ink)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, 22, 22);
    lv_arc_set_bg_angles(arc, 150, 390);
    lv_arc_set_angles(arc, 150, 150);
    lv_obj_set_style_arc_color(arc, color(ink), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    static const lv_point_precise_t points[] = {{10, 11}, {15, 5}};
    lv_obj_t *needle = lv_line_create(parent);
    lv_line_set_points(needle, points, 2);
    lv_obj_set_pos(needle, x, y);
    lv_obj_set_style_line_color(needle, color(ink), 0);
    lv_obj_set_style_line_width(needle, 2, 0);
    lv_obj_set_style_line_rounded(needle, true, 0);
    rounded_box(parent, x + 8, y + 9, 5, 5, ink, 3);
}

static void animation_icon(lv_obj_t *parent, int x, int y, uint32_t ink)
{
    // 两栋沿街建筑、窗户和楼下经过的小车组成像素街角。
    const int buildings[][4] = {{1, 5, 10, 8}, {11, 1, 10, 12}};
    for (unsigned i = 0; i < sizeof buildings / sizeof *buildings; ++i) {
        const int *rect = buildings[i];
        lv_obj_t *facade = rounded_box(parent, x + rect[0], y + rect[1], rect[2], rect[3], BG, 1);
        lv_obj_set_style_bg_opa(facade, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(facade, 1, 0);
        lv_obj_set_style_border_color(facade, color(ink), 0);
    }
    box(parent, x, y + 5, 11, 1, ink);
    box(parent, x + 3, y + 8, 2, 2, ink);
    box(parent, x + 7, y + 8, 2, 5, ink);
    box(parent, x + 13, y + 3, 2, 2, ink);
    box(parent, x + 17, y + 3, 2, 2, ink);
    box(parent, x + 13, y + 7, 2, 2, ink);
    box(parent, x + 17, y + 7, 2, 2, ink);

    box(parent, x + 13, y + 14, 5, 1, ink);
    box(parent, x + 11, y + 15, 10, 2, ink);
    box(parent, x + 12, y + 17, 2, 1, ink);
    box(parent, x + 18, y + 17, 2, 1, ink);
    box(parent, x + 2, y + 16, 6, 1, ink);
}

static void game_icon(lv_obj_t *parent, int x, int y, uint32_t ink, uint32_t face)
{
    rounded_box(parent, x, y, 22, 14, ink, 5);
    box(parent, x + 4, y + 6, 7, 2, face);
    box(parent, x + 6, y + 4, 2, 6, face);
    box(parent, x + 13, y + 7, 2, 2, face);
    box(parent, x + 16, y + 4, 2, 2, face);
}

static void page_icons(void)
{
    rounded_box(body, 16, 240, 208, 32, SURFACE, 12);
    for (unsigned i = 0; i < sizeof(display_pages) / sizeof(*display_pages); ++i) {
        int x = 22 + 32 * i;
        uint8_t page = display_pages[i];
        bool active = page == preferences.page;
        uint32_t ink = active ? INK : MUTED;
        if (active) rounded_box(body, x, 243, 30, 26, ACCENT, 8);
        if (page == BADGE) identity_icon(body, x + 4, 247, ink);
        else if (page == QUOTA) quota_icon(body, x + 4, 246, ink);
        else if (page == PHOTO) label(body, LV_SYMBOL_IMAGE, x + 5, 246, &lv_font_montserrat_20, ink);
        else if (page == CITY) animation_icon(body, x + 4, 247, ink);
        else game_icon(body, x + 4, 249, ink, active ? ACCENT : SURFACE);
    }
    box(body, 185, 249, 1, 14, TRACK);
    label(body, LV_SYMBOL_SETTINGS, 194, 246, &lv_font_montserrat_20, MUTED);
}

static void empty_state(void)
{
    rounded_box(body, 16, 16, AVATAR_WIDTH, AVATAR_HEIGHT, SURFACE, 10);
    person_placeholder(body);
    lv_obj_t *heading = label(body, "身份卡", 142, 16, &passport_heading, TEXT);
    lv_obj_set_width(heading, 82);
    lv_obj_t *hint = label(body, "添加资料", 142, 54, &passport_caption, ACCENT);
    lv_obj_set_width(hint, 82);
}

// 距离最近到期时间：3 天外正常，1–3 天提醒，24 小时内（含已到期）警示。
static uint32_t expiry_color(uint32_t expires_at, time_t now)
{
    if (!expires_at) return MUTED;
    int64_t remaining = (int64_t)expires_at - now;
    return remaining <= 86400 ? DANGER : remaining <= 3 * 86400 ? LOW : TEXT;
}

static void format_quota_duration(lv_obj_t *duration, uint16_t minutes)
{
    if (minutes % 1440 == 0) lv_label_set_text_fmt(duration, "%u 天", minutes / 1440);
    else if (minutes % 60 == 0) lv_label_set_text_fmt(duration, "%u 小时", minutes / 60);
    else lv_label_set_text_fmt(duration, "%u 分钟", minutes);
}

static bool quota_window_expired(uint32_t reset_at, time_t now)
{
    return now >= 1700000000 && reset_at && now >= reset_at;
}

static void format_quota_reset(lv_obj_t *text, uint32_t reset_at, time_t now)
{
    if (quota_window_expired(reset_at, now)) {
        lv_label_set_text(text, "等待刷新");
        return;
    }
    struct tm reset = local_clock(reset_at);
    lv_label_set_text_fmt(text, "%02d/%02d %02d:%02d 重置", reset.tm_mon + 1,
                         reset.tm_mday, reset.tm_hour, reset.tm_min);
}

static bool quota_fresh(uint32_t updated_at, time_t now, sync_status_t status)
{
    return quota_connected() && !status.quota_failed && updated_at &&
        now >= updated_at && now - updated_at < 180;
}

static const char *quota_sync_hint(sync_status_t status)
{
    if (!quota_ble_enabled()) return "蓝牙已关闭";
    if (!quota_connected()) return "未连接";
    return status.quota_failed ? "同步失败" : "等待同步";
}

static void render_quota_sync(uint32_t updated_at, time_t now, sync_status_t status)
{
    if (!updated_at) return;
    bool fresh = quota_fresh(updated_at, now, status);
    struct tm updated = local_clock(updated_at);
    label(body, fresh ? LV_SYMBOL_OK : LV_SYMBOL_LOOP, 18, 222, &lv_font_montserrat_14, fresh ? ACCENT : LOW);
    lv_obj_t *sync = label(body, "", 40, 220, &passport_caption, MUTED);
    lv_label_set_text_fmt(sync, "%s · %02d:%02d", fresh ? "更新于" : quota_sync_hint(status),
                         updated.tm_hour, updated.tm_min);
}

static uint32_t quota_window_ink(quota_window_t window, bool fresh, time_t now)
{
    if (!fresh || quota_window_expired(window.reset_at, now)) return MUTED;
    return window.remaining <= 15 ? LOW : ACCENT;
}

static void render_quota_amount(lv_obj_t *card, const char *amount, int top, uint32_t ink)
{
    lv_obj_t *digits = label(card, amount, 0, 0, &passport_quota, ink);
    lv_obj_t *sign = label(card, "%", 0, 0, &lv_font_montserrat_40, ink);
    lv_obj_update_layout(card);
    int digits_width = lv_obj_get_width(digits);
    int x = (lv_obj_get_width(card) - digits_width - 5 - lv_obj_get_width(sign)) / 2;
    lv_obj_set_pos(digits, x, top);
    lv_obj_set_pos(sign, x + digits_width + 5, top + passport_quota.line_height - passport_quota.base_line
                   - (lv_font_montserrat_40.line_height - lv_font_montserrat_40.base_line));
}

static void render_quota_focus(quota_snapshot_t value, size_t count, time_t now, sync_status_t status)
{
    if (!count) {
        lv_obj_t *card = rounded_box(body, 16, 47, 208, 167, SURFACE, 12);
        render_quota_amount(card, "--", 39, MUTED);
        centered(body, quota_sync_hint(status), 170, &passport_caption, MUTED);
        return;
    }

    bool single = count == 1;
    int y = single ? 47 : 44;
    for (size_t i = 0; i < 2; ++i) {
        quota_window_t window = value.windows[i];
        if (!window.minutes) continue;
        lv_obj_t *card = rounded_box(body, 16, y, 208, single ? 167 : 83, SURFACE, 12);
        lv_obj_t *duration = label(card, "", single ? 0 : 14, single ? 17 : 23, &passport_caption, MUTED);
        format_quota_duration(duration, window.minutes);
        const uint32_t ink = quota_window_ink(window, quota_fresh(value.updated_at, now, status), now);
        if (single) {
            char digits[4];
            snprintf(digits, sizeof digits, "%u", (unsigned)window.remaining);
            render_quota_amount(card, digits, 39, ink);
        } else {
            lv_obj_t *remaining = label(card, "", 74, 4, &lv_font_montserrat_40, ink);
            lv_label_set_text_fmt(remaining, "%u%%", window.remaining);
            lv_obj_set_width(remaining, 120);
            lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_RIGHT, 0);
        }
        int bar_y = single ? 121 : 52;
        rounded_box(card, 14, bar_y, 180, 4, TRACK, 2);
        if (window.remaining)
            rounded_box(card, 14, bar_y, (window.remaining * 180 + 99) / 100, 4, ink, 2);
        lv_obj_t *reset = label(card, "重置时间待同步", single ? 0 : 14, single ? 135 : 62, &passport_caption, MUTED);
        if (window.reset_at) format_quota_reset(reset, window.reset_at, now);
        if (single) {
            lv_obj_set_width(duration, 208);
            lv_obj_set_style_text_align(duration, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(reset, 208);
            lv_obj_set_style_text_align(reset, LV_TEXT_ALIGN_CENTER, 0);
        }
        y += 91;
    }
}

static void render_quota(quota_snapshot_t value, time_t now)
{
    label(body, "Codex", 16, 3, &lv_font_montserrat_28, TEXT);
    sync_status_t status = quota_sync_status(NULL);
    lv_obj_t *subtitle = label(body, "剩余额度", 128, 12, &passport_caption, MUTED);
    lv_obj_set_width(subtitle, 96);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_RIGHT, 0);
    if (quota_connected() && status.tasks_known && status.active) {
        lv_label_set_text_fmt(subtitle, "进行中 %u", (unsigned)status.active);
        lv_obj_set_style_text_color(subtitle, color(ACCENT), 0);
    }
    size_t count = (value.windows[0].minutes > 0) + (value.windows[1].minutes > 0);
    if (preferences.quota_focus) {
        render_quota_focus(value, count, now, status);
        render_quota_sync(value.updated_at, now, status);
        page_icons();
        return;
    }
    int y = 44;
    for (size_t i = 0; i < 2; ++i) {
        quota_window_t window = value.windows[i];
        if (!window.minutes) continue;
        bool large = count == 1;
        uint32_t background = SURFACE;
        uint32_t ink = quota_window_ink(window, quota_fresh(value.updated_at, now, status), now);
        uint32_t detail = MUTED;
        int height = large ? 116 : 55;
        lv_obj_t *card = rounded_box(body, 16, y, 208, height, background, 12);
        lv_obj_t *duration = label(card, "", 14, large ? 14 : 7, &passport_caption, detail);
        format_quota_duration(duration, window.minutes);
        lv_obj_t *remaining = label(card, "", 94, large ? 18 : 0,
                                    large ? &lv_font_montserrat_40 : &lv_font_montserrat_28,
                                    ink);
        lv_label_set_text_fmt(remaining, "%u%%", window.remaining);
        lv_obj_set_width(remaining, 100);
        lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_RIGHT, 0);
        int bar_y = large ? 76 : 30;
        rounded_box(card, 14, bar_y, 180, 5, TRACK, 2);
        if (window.remaining)
            rounded_box(card, 14, bar_y, (window.remaining * 180 + 99) / 100, 5,
                        ink, 2);
        if (window.reset_at) {
            lv_obj_t *reset_label = label(card, "", 14, large ? 91 : 38, &passport_caption, detail);
            format_quota_reset(reset_label, window.reset_at, now);
        }
        y += height + 8;
    }
    if (value.updated_at) {
        render_quota_sync(value.updated_at, now, status);
    } else {
        rounded_box(body, 16, 44, 208, 116, SURFACE, 12);
        centered(body, LV_SYMBOL_BLUETOOTH, 61, &lv_font_montserrat_28, ACCENT);
        centered(body, quota_sync_hint(status), 104, &passport_font, TEXT);
        centered(body, quota_connected() ? "到 Mac 查看同步状态" :
                 quota_ble_enabled() ? "设置中连接 Mac" : "设置中开启蓝牙", 134, &passport_caption, MUTED);
    }
    reset_snapshot_t resets = quota_resets();
    bool reset_fresh = quota_connected() && resets.updated_at && now - resets.updated_at < 180;
    rounded_box(body, 16, 170, 208, 47, SURFACE, 12);
    label(body, "重置卡", 28, 176, &passport_font, TEXT);
    lv_obj_t *cards = label(body, "", 148, 174, &lv_font_montserrat_20, reset_fresh ? ACCENT : MUTED);
    lv_obj_set_width(cards, 62); lv_obj_set_style_text_align(cards, LV_TEXT_ALIGN_RIGHT, 0);
    if (resets.known) lv_label_set_text_fmt(cards, "%u", resets.count);
    else lv_label_set_text(cards, "--");
    lv_obj_t *expiry = label(body, "", 28, 199, &passport_caption, expiry_color(resets.expires_at, now));
    if (!resets.known) lv_label_set_text(expiry, "有效期待同步");
    else if (!resets.count) lv_label_set_text(expiry, "暂无可用重置卡");
    else if (!resets.expires_at) lv_label_set_text(expiry, "有效期未提供");
    else if (now >= resets.expires_at) lv_label_set_text(expiry, "到期信息待刷新");
    else {
        struct tm date = local_clock(resets.expires_at);
        lv_label_set_text_fmt(expiry, "%s %02d/%02d %02d:%02d 到期", resets.complete ? "最近" : "最近已知",
                             date.tm_mon + 1, date.tm_mday, date.tm_hour, date.tm_min);
    }
    page_icons();
}

static void render_storage(void)
{
    label(body, "存储空间", 16, 3, &passport_heading, TEXT);
    storage_usage_t value;
    if (storage_usage(&value) != ESP_OK) { centered(body, "存储信息读取失败", 116, &passport_font, LOW); return; }
    lv_obj_t *total = label(body, "", 16, 42, &lv_font_montserrat_28, TEXT);
    lv_label_set_text_fmt(total, "%u MiB", (unsigned)(value.total / 1048576));
    label(body, "硬件闪存", 150, 56, &passport_caption, MUTED);
    uint32_t used = value.firmware + value.badge + value.photo;
    rounded_box(body, 16, 83, 208, 7, TRACK, 3);
    int system_width = (uint64_t)value.firmware * 208 / value.total;
    int asset_width = (uint64_t)(value.badge + value.photo) * 208 / value.total;
    if (system_width) box(body, 16, 83, system_width, 7, MUTED);
    if (asset_width) box(body, 16 + system_width, 83, asset_width, 7, ACCENT);
    const char *names[] = {"运行固件", "个人内容", "系统与预留"};
    uint32_t sizes[] = {value.firmware, value.badge + value.photo, value.total - used};
    for (size_t i = 0; i < 3; ++i) {
        label(body, names[i], 20, 106 + i * 31, &passport_font, i == 1 ? ACCENT : MUTED);
        lv_obj_t *amount = label(body, "", 125, 108 + i * 31, &lv_font_montserrat_14, TEXT);
        lv_obj_set_width(amount, 94); lv_obj_set_style_text_align(amount, LV_TEXT_ALIGN_RIGHT, 0);
        if (sizes[i] >= 1048576) lv_label_set_text_fmt(amount, "%u.%02u MiB", (unsigned)(sizes[i] / 1048576), (unsigned)((uint64_t)(sizes[i] % 1048576) * 100 / 1048576));
        else lv_label_set_text_fmt(amount, "%u KiB", (unsigned)((sizes[i] + 1023) / 1024));
    }
    centered(body, "图片区 1 MiB · 最多 3 张", 213, &passport_caption, MUTED);
    lv_obj_t *pictures = centered(body, "", 236, &passport_caption, MUTED);
    lv_label_set_text_fmt(pictures, "身份卡 %uK · 照片 %uK", (unsigned)((value.badge + 1023) / 1024), (unsigned)((value.photo + 1023) / 1024));
}

static const char *edit_title(int setting)
{
    switch (setting) {
    case WIFI_POWER: return "日常 Wi-Fi";
    case BLE_POWER: return "蓝牙";
    case BRIGHTNESS: return "亮度";
    case VOLUME: return "音量";
    case TASK_CHIME_VOLUME: return "提示音量";
    case SLEEP: return "息屏";
    case POWER_MODE: return "使用模式";
    case DEEP_SLEEP_SETTING: return "自动休眠";
    case MUSIC_ENABLED_SETTING: return "开机播放";
    case MUSIC_SELECTION_SETTING: return "旋律";
    default: return "设置";
    }
}

static int edit_count(int setting)
{
    if (setting == SLEEP) return sizeof sleep_options / sizeof sleep_options[0];
    if (setting == DEEP_SLEEP_SETTING) return sizeof deep_sleep_options / sizeof deep_sleep_options[0];
    if (setting == MUSIC_SELECTION_SETTING) return MUSIC_PRESET_COUNT + music_status().has_custom;
    return 2;
}

static const char *edit_option(int setting, int value)
{
    static const char *sleep_names[] = {"30 秒", "1 分钟", "3 分钟", "5 分钟", "常亮"};
    if (setting == SLEEP) return sleep_names[value];
    static const char *deep_names[] = {"5 分钟", "15 分钟", "30 分钟"};
    if (setting == DEEP_SLEEP_SETTING) return deep_names[value];
    if (setting == POWER_MODE) return value == POWER_DESKTOP ? "桌面常连" : "随身省电";
    if (setting == MUSIC_SELECTION_SETTING) return value == MUSIC_CUSTOM ? "自定义" : music_presets[value].name;
    return value ? "开启" : "关闭";
}

static void render_editor(void)
{
    label(body, edit_title(editing), 16, 3, &passport_heading, TEXT);
    bool numeric = editing == BRIGHTNESS || editing == VOLUME || editing == TASK_CHIME_VOLUME;
    if (numeric) {
        const char *icon = editing == BRIGHTNESS ? LV_SYMBOL_EYE_OPEN : edit_draft ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE;
        if (editing == BRIGHTNESS) sun_icon(body, 106, 51, 28, ACCENT);
        else centered(body, icon, 51, &lv_font_montserrat_28, ACCENT);
        lv_obj_t *value = centered(body, "", 98, &lv_font_montserrat_40, TEXT);
        lv_label_set_text_fmt(value, "%d%%", edit_draft);
        rounded_box(body, 26, 163, 188, 6, TRACK, 3);
        if (edit_draft) rounded_box(body, 26, 163, edit_draft * 188 / 100, 6, ACCENT, 3);
        label(body, LV_SYMBOL_DOWN, 26, 190, &lv_font_montserrat_14, MUTED);
        label(body, LV_SYMBOL_UP, 202, 190, &lv_font_montserrat_14, MUTED);
        lv_obj_t *saved = centered(body, "", 190, &passport_caption, MUTED);
        if (editing == TASK_CHIME_VOLUME && edit_saved == MUSIC_VOLUME_FOLLOW)
            lv_label_set_text(saved, "当前随系统");
        else lv_label_set_text_fmt(saved, "当前 %d%%", edit_saved);
    } else {
        int count = edit_count(editing), height = count > 4 ? 31 : count == 4 ? 39 : 43;
        int top = count == 2 ? 65 : 44;
        for (int i = 0; i < count; ++i) {
            int y = top + i * (height + 7);
            focus_card(body, y, height, edit_draft == i);
            label(body, edit_option(editing, i), 30, y + (height - 20) / 2, &passport_font, TEXT);
            if (i == edit_saved) label(body, LV_SYMBOL_OK, 196, y + (height - 14) / 2,
                                       &lv_font_montserrat_14, ACCENT);
        }
        if (editing == POWER_MODE)
            centered(body, edit_draft == POWER_DESKTOP ? "息屏后继续同步" : "休眠后暂停同步", 195, &passport_caption, MUTED);
        if (editing == DEEP_SLEEP_SETTING) centered(body, "随身模式生效", 211, &passport_caption, MUTED);
    }
    if (notice) centered(body, notice, 235, &passport_caption, LOW);
}

static void render_settings(const web_status_t *web, int passkey)
{
    boot_music_status_t music = music_status();
    back_hint();
    if (editing >= 0) { render_editor(); return; }
    if (storage_visible) { render_storage(); return; }
    if (about_visible) {
        label(body, "关于", 16, 3, &passport_heading, TEXT);
        centered(body, "FoloToy", 61, &lv_font_montserrat_28, TEXT);
        centered(body, "AI Passport", 96, &lv_font_montserrat_20, MUTED);
        centered(body, "v" PASSPORT_RELEASE_VERSION, 143, &lv_font_montserrat_28, ACCENT);
        centered(body, "更新于 " PASSPORT_RELEASE_UPDATED_AT, 182, &passport_caption, MUTED);
        centered(body, "ESP32-C3  /  8 MiB", 220, &lv_font_montserrat_14, MUTED);
        return;
    }
    if (maintenance_action) {
        bool reset = maintenance_action == MAINTENANCE_RESET;
        label(body, reset ? "清除数据与设置" : "解除配对", 16, 3, &passport_heading, TEXT);
        centered(body, LV_SYMBOL_WARNING, 49, &lv_font_montserrat_28, LOW);
        lv_obj_t *description = label(body, reset ? "清除个人内容、设置与配对。保留当前固件。" : "断开 Mac 并清除配对记录。", 28, 99, &passport_font, TEXT);
        lv_obj_set_width(description, 184);
        lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, 0);
        rounded_box(body, 16, 180, 208, 36, confirm_selected ? SURFACE : ACCENT, 10);
        rounded_box(body, 16, 222, 208, 36, confirm_selected ? LOW : SURFACE, 10);
        centered(body, "取消", 189, &passport_font, confirm_selected ? TEXT : INK);
        centered(body, reset ? "清除并重启" : "解除并重启", 231, &passport_font, confirm_selected ? INK : LOW);
        if (notice) centered(body, notice, 157, &passport_caption, LOW);
        return;
    }
    if (sound_panel) {
        bool chime = sound_panel == SOUND_CHIME;
        label(body, chime ? "提示音" : "开机音乐", 16, 3, &passport_heading, TEXT);
        label(body, chime ? LV_SYMBOL_BELL : LV_SYMBOL_AUDIO, 203, 5, &lv_font_montserrat_20, MUTED);
        const char *boot_entries[] = {"开机播放", "旋律", "试听"};
        const char *chime_entries[] = {"开启提示音", "随系统音量", "提示音量", "试听"};
        const char *boot_icons[] = {LV_SYMBOL_POWER, LV_SYMBOL_AUDIO, LV_SYMBOL_PLAY};
        const char *chime_icons[] = {LV_SYMBOL_BELL, LV_SYMBOL_LOOP, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_PLAY};
        uint8_t volume = chime ? music_completion_volume() : MUSIC_VOLUME_FOLLOW;
        int count = chime ? CHIME_MENU_COUNT : MUSIC_MENU_COUNT;
        for (int i = 0; i < count; ++i) {
            int y = 43 + (chime ? 49 : 58) * i;
            bool active = i == music_menu;
            int height = chime ? 43 : 50;
            int inset = (height - 43) / 2;
            focus_card(body, y, height, active);
            label(body, chime ? chime_icons[i] : boot_icons[i],
                  29, y + 14 + inset, &lv_font_montserrat_14, active ? ACCENT : MUTED);
            label(body, chime ? chime_entries[i] : boot_entries[i], 52, y + 12 + inset, &passport_font, TEXT);
            if (i == MUSIC_ENABLE) {
                toggle_control(body, 175, y + (height - 20) / 2, chime ? music_completion_enabled() : music.enabled);
            } else if (chime && i == CHIME_FOLLOW) {
                toggle_control(body, 175, y + (height - 20) / 2, volume == MUSIC_VOLUME_FOLLOW);
            } else if ((!chime && i == MUSIC_MELODY) || (chime && i == CHIME_LEVEL)) {
                lv_obj_t *value = label(body, "", 152, y + 14 + inset,
                                        chime ? &lv_font_montserrat_14 : &passport_caption, ACCENT);
                lv_obj_set_width(value, 61);
                lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
                if (chime) lv_label_set_text_fmt(value, "%u%%", (unsigned)(volume == MUSIC_VOLUME_FOLLOW ? music_volume() : volume));
                else lv_label_set_text(value, music.selection == MUSIC_CUSTOM ? "自定义" : music_presets[music.selection].name);
            }
        }
        if (notice) centered(body, notice, chime ? 238 : 223, &passport_caption, LOW);
        return;
    }
    if (web_visible) {
        label(body, "网页设置", 16, 3, &passport_heading, TEXT);
        label(body, web->hotspot ? LV_SYMBOL_WIFI : LV_SYMBOL_HOME, 201, 7,
              &lv_font_montserrat_20, ACCENT);
        bool join = web->hotspot && web_join_qr;
        centered(body, join ? "1  扫码连接卡片" : web->hotspot ? "2  扫码打开网页" : "扫码打开 · Mac 输入网址",
                 36, &passport_caption, MUTED);
        char payload[128];
        if (join) snprintf(payload, sizeof payload, "WIFI:T:WPA;S:%s;P:%s;;", web->ssid, web->password);
        else snprintf(payload, sizeof payload, "%s", web->url);
        if (payload[0]) {
            lv_obj_t *qr = lv_qrcode_create(body);
            lv_qrcode_set_size(qr, 116);
            lv_obj_set_style_border_width(qr, 6, 0);
            lv_obj_set_style_border_color(qr, lv_color_white(), 0);
            lv_obj_set_pos(qr, 56, 62);
            lv_result_t result = lv_qrcode_update(qr, payload, strlen(payload));
            if (result != LV_RESULT_OK) {
                ESP_LOGE("passport", "QR generation failed (kind=%s, bytes=%u)",
                         join ? "wifi" : "url", (unsigned)strlen(payload));
                lv_obj_delete(qr);
                centered(body, LV_SYMBOL_WARNING, 105, &lv_font_montserrat_28, LOW);
            }
        } else centered(body, "连接已断开", 112, &passport_font, LOW);
        if (join) {
            centered(body, web->ssid, 189, &lv_font_montserrat_14, TEXT);
            label(body, "密码", 39, 212, &passport_caption, MUTED);
            label(body, web->password, 84, 208, &lv_font_montserrat_20, ACCENT);
            centered(body, web->short_url, 235, &lv_font_montserrat_14, TEXT);
        } else {
            centered(body, web->short_url, 198, &lv_font_montserrat_14, TEXT);
            if (!web->hotspot) {
                label(body, "访问码", 32, 229, &passport_caption, MUTED);
                label(body, web->code, 94, 221, &lv_font_montserrat_28, ACCENT);
            } else centered(body, "手机和 Mac 均可编辑", 228, &passport_caption, MUTED);
        }
        return;
    }
    if (pairing_visible) {
        label(body, "连接 Mac", 16, 5, &passport_heading, TEXT);
        bool connected = quota_connected();
        rounded_box(body, 16, 42, 208, 157, SURFACE, 12);
        rounded_box(body, 90, 58, 60, 60, ACCENT, 20);
        centered(body, connected ? LV_SYMBOL_OK : LV_SYMBOL_BLUETOOTH, 74, &lv_font_montserrat_28, INK);
        lv_obj_t *code = centered(body, "", 137, &lv_font_montserrat_28, TEXT);
        if (passkey >= 0) lv_label_set_text_fmt(code, "%03d %03d", passkey / 1000, passkey % 1000);
        else lv_label_set_text(code, connected ? LV_SYMBOL_OK : "--- ---");
        centered(body, connected ? "已连接" : passkey >= 0 ? "在 Mac 上输入配对码" : "等待 Mac 连接",
                 218, &passport_caption, MUTED);
        return;
    }

    if (bluetooth_visible) {
        label(body, "Mac 蓝牙", 16, 3, &passport_heading, TEXT);
        const char *state = !quota_ble_enabled() ? "关闭" : quota_connected() ? "已连接" : quota_paired() ? "未连接" : "未配对";
        lv_obj_t *status = label(body, state, 155, 9, &passport_caption, quota_connected() ? ACCENT : MUTED);
        lv_obj_set_width(status, 61); lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
        const char *names[] = {"蓝牙开关", "配对 Mac", "解除配对"};
        const char *icons[] = {LV_SYMBOL_POWER, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_CLOSE};
        for (int i = 0; i < 3; ++i) {
            int y = 49 + i * 55;
            focus_card(body, y, 47, i == bluetooth_menu);
            label(body, icons[i], 29, y + 16, &lv_font_montserrat_14, i == bluetooth_menu ? ACCENT : MUTED);
            label(body, names[i], 54, y + 13, &passport_font, i == 2 ? LOW : TEXT);
            if (!i) toggle_control(body, 175, y + 14, quota_ble_enabled());
            else label(body, LV_SYMBOL_RIGHT, 202, y + 16, &lv_font_montserrat_14, MUTED);
        }
        if (notice) centered(body, notice, 230, &passport_caption, LOW);
        return;
    }
    if (power_visible) {
        label(body, "省电", 16, 3, &passport_heading, TEXT);
        const char *names[] = {"使用模式", "自动休眠"};
        for (int i = 0; i < 2; ++i) {
            int y = 50 + i * 73;
            focus_card(body, y, 62, i == power_menu);
            label(body, names[i], 30, y + 9, &passport_font, TEXT);
            lv_obj_t *value = label(body, "", 30, y + 34, &passport_caption, MUTED);
            if (!i) lv_label_set_text(value, preferences.power_mode == POWER_DESKTOP ? "桌面常连" : "随身省电");
            else lv_label_set_text_fmt(value, "%u 分钟 · 随身模式", preferences.deep_sleep_seconds / 60);
            label(body, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_REFRESH, 202, y + 23, &lv_font_montserrat_14, MUTED);
        }
        centered(body, preferences.power_mode == POWER_DESKTOP ? "息屏后继续同步" : "休眠后暂停无线同步", 213, &passport_caption, MUTED);
        if (notice) centered(body, notice, 235, &passport_caption, LOW);
        return;
    }

    label(body, settings_group ? settings_titles[settings_group - 1] : "设置", 16, 3, &passport_heading, TEXT);
    if (!settings_group) {
        const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_SETTINGS};
        const char *levels = "";
        const char *values[] = {"网页设置 / Mac", levels, preferences.power_mode == POWER_DESKTOP ? "桌面常连" : "随身省电"};
        for (int i = 0; i < 3; ++i) {
            int y = 107 + i * 49;
            bool active = i + 1 == selection;
            focus_card(body, y, 43, active);
            if (i == 1) sun_icon(body, 30, y + 11, 20, active ? ACCENT : MUTED);
            else label(body, icons[i], 30, y + 12, &lv_font_montserrat_20, active ? ACCENT : MUTED);
            label(body, settings_titles[i], 64, y + 3, &passport_font, TEXT);
            if (i == 1) {
                sun_icon(body, 64, y + 24, 15, MUTED);
                lv_obj_t *brightness = label(body, "", 85, y + 24, &lv_font_montserrat_14, MUTED);
                lv_label_set_text_fmt(brightness, "%u%%", preferences.brightness);
                label(body, preferences.volume ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE, 137, y + 24, &lv_font_montserrat_14, MUTED);
                lv_obj_t *volume = label(body, "", 157, y + 24, &lv_font_montserrat_14, MUTED);
                lv_label_set_text_fmt(volume, "%u%%", preferences.volume);
            } else label(body, values[i], 64, y + 24, &passport_caption, MUTED);
            label(body, LV_SYMBOL_RIGHT, 202, y + 16, &lv_font_montserrat_14, active ? ACCENT : INACTIVE);
        }
        focus_card(body, 41, 60, selection == 0);
        label(body, LV_SYMBOL_SD_CARD, 30, 54, &lv_font_montserrat_20, selection == 0 ? ACCENT : MUTED);
        label(body, "存储", 64, 47, &passport_font, TEXT);
        storage_usage_t usage;
        lv_obj_t *amount = label(body, "--", 112, 51, &lv_font_montserrat_14, MUTED);
        lv_obj_set_width(amount, 100); lv_obj_set_style_text_align(amount, LV_TEXT_ALIGN_RIGHT, 0);
        if (storage_usage(&usage) == ESP_OK) {
            uint32_t pictures = usage.badge + usage.photo;
            lv_label_set_text_fmt(amount, "%u MiB", (unsigned)(usage.total / 1048576));
            lv_obj_t *assets = label(body, "", 64, 71, &passport_caption, MUTED);
            lv_label_set_text_fmt(assets, "图片 %uK / %u MiB", (unsigned)((pictures + 1023) / 1024), (unsigned)(usage.assets_capacity / 1048576));
            rounded_box(body, 64, 92, 148, 4, TRACK, 2);
            int width = (uint64_t)pictures * 148 / usage.assets_capacity;
            if (width) rounded_box(body, 64, 92, width, 4, ACCENT, 2);
        }
        return;
    }

    const char *entries[] = {"网页设置", "Mac 蓝牙", "亮度", "音量", "开机音乐", "息屏", "省电", "解除配对", "清除数据与设置", "日常 Wi-Fi", "蓝牙", "", "", "", "关于", "任务完成提示音"};
    const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_BLUETOOTH,
                          LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_AUDIO, LV_SYMBOL_POWER,
                          LV_SYMBOL_CHARGE, LV_SYMBOL_CLOSE, LV_SYMBOL_REFRESH, LV_SYMBOL_WIFI, LV_SYMBOL_BLUETOOTH, "", "", "", LV_SYMBOL_LIST, LV_SYMBOL_BELL};
    enum { ROW_TOP = 41, ROW_HEIGHT = 43, ROW_STEP = 49, LIST_HEIGHT = 4 * ROW_STEP + ROW_HEIGHT / 2 };
    int count = settings_counts[settings_group - 1];
    int content_height = (count - 1) * ROW_STEP + ROW_HEIGHT;
    bool scrolls = content_height > LIST_HEIGHT;
    lv_obj_t *list = body;
    int origin = ROW_TOP;
    if (scrolls) {
        // 视窗保留四行半。按键移动时，只在需要完整显示焦点的情况下调整偏移。
        int focused_top = selection * ROW_STEP;
        if (focused_top < settings_scroll_y) settings_scroll_y = focused_top;
        else if (focused_top + ROW_HEIGHT > settings_scroll_y + LIST_HEIGHT)
            settings_scroll_y = focused_top + ROW_HEIGHT - LIST_HEIGHT;
        list = box(body, 0, ROW_TOP, 228, LIST_HEIGHT, BG);
        origin = -settings_scroll_y;
    }
    for (int row = 0; row < count; ++row) {
        int i = settings_items[settings_group - 1][row];
        int y = origin + ROW_STEP * row;
        if (scrolls && (y + ROW_HEIGHT <= 0 || y >= LIST_HEIGHT)) continue;
        bool active = row == selection;
        uint32_t ink = i == RESET ? LOW : TEXT;
        focus_card(list, y, ROW_HEIGHT, active);
        if (i == BRIGHTNESS) sun_icon(list, 29, y + 13, 17, active ? ACCENT : MUTED);
        else label(list, i == VOLUME && !preferences.volume ? LV_SYMBOL_MUTE : icons[i],
                   29, y + 14, &lv_font_montserrat_14, active ? ACCENT : MUTED);
        // 16 px 中文和 14 px 小字/图标的基线统一在本行第 27 px。
        label(list, entries[i], 52, y + (i == TASK_CHIME ? 14 : 12),
              i == TASK_CHIME ? &passport_caption : &passport_font, ink);
        lv_obj_t *value = label(list, "", 152, y + 14, &lv_font_montserrat_14, active ? ACCENT : MUTED);
        lv_obj_set_width(value, 61);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        if (i == WIFI_POWER || i == BLE_POWER) {
            toggle_control(list, 175, y + 12, i == WIFI_POWER ? network_enabled() : quota_ble_enabled());
        } else if (i == TASK_CHIME) {
            uint8_t volume = music_completion_volume();
            bool enabled = music_completion_enabled();
            if (!enabled || volume == MUSIC_VOLUME_FOLLOW) {
                lv_obj_set_style_text_font(value, &passport_caption, 0);
                lv_label_set_text(value, enabled ? "随系统" : "关闭");
            } else lv_label_set_text_fmt(value, "%u%%", (unsigned)volume);
        } else if (i == BRIGHTNESS) lv_label_set_text_fmt(value, "%u%%", preferences.brightness);
        else if (i == VOLUME) lv_label_set_text_fmt(value, "%u%%", preferences.volume);
        else if (i == BOOT_MUSIC) {
            lv_obj_set_style_text_font(value, &passport_caption, 0);
            lv_label_set_text(value, music.enabled ?
                              (music.selection == MUSIC_CUSTOM ? "自定义" : music_presets[music.selection].name) : "关闭");
        } else if (i == POWER_MODE) {
            lv_obj_set_style_text_font(value, &passport_caption, 0);
            lv_label_set_text(value, preferences.power_mode == POWER_DESKTOP ? "桌面" : "随身");
        } else if (i == SLEEP) {
            if (preferences.sleep_seconds) lv_label_set_text_fmt(value, "%us", preferences.sleep_seconds);
            else {
                lv_obj_set_style_text_font(value, &passport_caption, 0);
                lv_label_set_text(value, "常亮");
            }
        } else if (i != RESET) lv_label_set_text(value, LV_SYMBOL_RIGHT);
    }
    if (scrolls) {
        rounded_box(body, 230, ROW_TOP, 2, LIST_HEIGHT, TRACK, 1);
        rounded_box(body, 230, ROW_TOP + LIST_HEIGHT * settings_scroll_y / content_height,
                    2, LIST_HEIGHT * LIST_HEIGHT / content_height, MUTED, 1);
    }
    if (notice) {
        if (scrolls) lv_obj_align(label(body, notice, 0, 0, &passport_caption, LOW),
                                  LV_ALIGN_TOP_RIGHT, -16, 8);
        else centered(body, notice, 234, &passport_caption, LOW);
    }
}

static void expire_city_title(int64_t now)
{
    if (city_title && now >= city_title_until) {
        lv_obj_delete(city_title);
        city_title = NULL;
    }
}

static void city_render_event(lv_event_t *event)
{
    if (!city_render.samples) return;
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        city_render.render_at = esp_timer_get_time();
        return;
    }
    if (city_render.skip_first) {
        city_render.skip_first = false;
        return;
    }
    unsigned elapsed = esp_timer_get_time() - city_render.render_at;
    city_render.total_us += elapsed;
    if (elapsed > city_render.peak_us) city_render.peak_us = elapsed;
    if (--city_render.samples == 0) {
        ESP_LOGI("passport", "Scene %d: partial redraw mean=%u us, peak=%u us, frame interval=%u us",
                 city.scene, city_render.total_us / 50, city_render.peak_us, CITY_FRAME_US);
    }
}

static void update_city_frame(time_t wall, int64_t now)
{
    if (now < next_city_frame) return;
    // 保持固定的更新节奏，错过的帧直接跳过，避免连续补绘。
    next_city_frame = now + CITY_FRAME_US - (now - next_city_frame) % CITY_FRAME_US;
    expire_city_title(now);
    pixel_city_update(&city, wall >= 1700000000 ? local_clock(wall).tm_hour : -1, now / 1000);
}

static int64_t photo_render_started;
static size_t photo_render_bytes;

static void photo_render_ready(lv_event_t *event)
{
    (void)event;
    photo_jpeg_finish_frame();
    if (!photo_render_started) return;
    unsigned elapsed = esp_timer_get_time() - photo_render_started;
    ESP_LOGI("passport", "Photo frame submitted: %u bytes, %u ms", (unsigned)photo_render_bytes,
             (elapsed + 500) / 1000);
    photo_render_started = 0;
}

static uint8_t selected_photo(void)
{
    photo_album_t album = photo_album_snapshot();
    return album.slots[album.selected];
}

static void render_photo_counter(void)
{
    if (esp_timer_get_time() >= photo_counter_until) return;
    const photo_album_t album = photo_album_snapshot();
    unsigned visible = 0, position = 0;
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i) {
        if (album.slots[i]) ++visible;
        if (i == album.selected) position = visible;
    }
    if (visible < 2 || !position) return;
    photo_counter = rounded_box(body, WIDTH - 72, HEIGHT - 44, 56, 28, BG, 10);
    lv_obj_set_style_bg_opa(photo_counter, LV_OPA_80, 0);
    lv_obj_t *counter = label(photo_counter, "", 0, 0, &lv_font_montserrat_14, TEXT);
    lv_label_set_text_fmt(counter, "%u / %u", position, visible);
    lv_obj_center(counter);
}

static void update_completion_toast(int64_t now, int64_t until)
{
    bool visible = quota_connected() && now < until && preferences.page != SETTINGS &&
        (preferences.page != GAME || game_screen == GAME_COVER);
    if (visible && !completion_toast) {
        completion_toast = rounded_box(root, 44, 253, 152, 26, SURFACE, 10);
        label(completion_toast, LV_SYMBOL_OK, 13, 5, &lv_font_montserrat_14, ACCENT);
        label(completion_toast, "Codex 任务完成", 35, 4, &passport_caption, TEXT);
    } else if (!visible && completion_toast) {
        lv_obj_delete(completion_toast);
        completion_toast = NULL;
    }
}

static void start_game(bool retry, int64_t now)
{
    if (retry) meow_game_reset(&game, (uint32_t)now, !game_tutorial_done);
    fish_hint_until = delivery_hint_until = 0;
    // 暂停切断一次发声；恢复后须重新发声，旧喵不能触发空中变招。
    game.seen_onset = game.jump_voice = 0;
    game_ready = game_voiced = false;
    last_game_frame = now;
    game_screen = music_game_capture(true) == ESP_OK ? GAME_PLAY : GAME_MIC_ERROR;
    dirty = true;
}

static void stop_game(uint8_t screen)
{
    music_game_capture(false);
    game_screen = screen;
    dirty = true;
}

static void save_game_record(void)
{
    uint32_t score = meow_distance(&game);
    if (score <= game_best) return;
    esp_err_t err = save_meow_best(score);
    if (err == ESP_OK) game_best = score;
    else {
        ESP_LOGE("passport", "Save game distance: %s", esp_err_to_name(err));
        notice = "保存失败";
    }
}

static bool tick_game(int64_t now)
{
    if (game_screen != GAME_PLAY || now - last_game_frame < GAME_FRAME_US) return false;
    unsigned elapsed = (now - last_game_frame) / 1000;
    last_game_frame += elapsed * 1000;
    meow_voice_t voice = music_game_voice();
    if (voice.failed) {
        stop_game(GAME_MIC_ERROR);
        return false;
    }
    if (!voice.ready) return false;
    game_voiced = voice.voiced;
    if (voice.onset != game.seen_onset) last_input = now;
    // 麦克风启动时角色停在原地，长时间丢帧后也不跳过大量路线。
    uint32_t fish = game.fish, deliveries = game.deliveries;
    bool was_teaching = game.teaching_left != 0;
    meow_game_step(&game, game_ready ? (elapsed > 100 ? 100 : elapsed) : 0, voice);
    game_ready = true;
    if (was_teaching && game.teaching_left == 0) {
        game_tutorial_done = true;
        esp_err_t err = save_meow_tutorial();
        if (err != ESP_OK) ESP_LOGE("passport", "Save game tutorial: %s", esp_err_to_name(err));
    }
    if (game.fish != fish) fish_hint_until = now + 750000;
    if (game.deliveries != deliveries) delivery_hint_until = now + 1000000;
    if (game.motion == MEOW_DONE) {
        stop_game(GAME_RESULT);
        save_game_record();
    }
    return true;
}

static void update_game_stats(void)
{
    char text[48];
    snprintf(text, sizeof text, "%u m", (unsigned)meow_distance(&game));
    set_label(game_score, text);
    snprintf(text, sizeof text, "鱼干 %u    送达 %u", (unsigned)game.fish, (unsigned)game.deliveries);
    set_label(game_totals, text);
}

static void update_game_view(void)
{
    game_paint = game;
    const meow_obstacle_t *next = &game.track[MEOW_CURRENT];
    const char *hint = game.rescue_used ? "救援已用" : "";
    if (game.teaching_left) hint = "短声跳　拖长伸爪";
    char delivered[32];
    if (!game_ready) hint = "麦克风准备中";
    else if (game.motion == MEOW_FALL) hint = game.retry_fall ? "抓住屋檐！" : "掉下去啦";
    else if (last_game_frame < delivery_hint_until) {
        snprintf(delivered, sizeof delivered, "已送达 × %u", (unsigned)game.deliveries);
        hint = delivered;
    } else if (game.motion == MEOW_REACH || game.motion == MEOW_PULL) hint = "伸爪！";
    else if (last_game_frame < fish_hint_until) hint = "鱼干到手！";
    else if (next->start - game.x / MEOW_UNIT < 112) {
        switch (next->kind) {
        case MEOW_BOX: hint = game.teaching_left ? "矮箱 · 短声" : "矮箱"; break;
        case MEOW_GAP: hint = game.teaching_left ? "宽缝 · 拖长" : "宽缝"; break;
        case MEOW_AWNING: hint = "雨棚 · 短声捡鱼"; break;
        case MEOW_CART: hint = "推车 · 看准再跳"; break;
        default: break;
        }
    }
    set_label(game_hint, hint);
    lv_color_t ink = color(game_voiced ? 0xffc078 : 0xffe3ad);
    if (lv_color_to_int(lv_obj_get_style_text_color(game_hint, 0)) != lv_color_to_int(ink))
        lv_obj_set_style_text_color(game_hint, ink, 0);
    update_game_stats();
    lv_obj_invalidate(game_view);
}

static void render_game(void)
{
    if (game_screen == GAME_COVER) {
        meow_game_reset(&game_paint, 0, true);
        game_paint.x = 182 * MEOW_UNIT;
        meow_view_bind(box(body, 16, 8, 208, 130, BG), &game_paint);
        centered(body, "喵喵快递", 144, &passport_game, 0xffe3ad);
        lv_obj_t *record = centered(body, "", 170, &passport_game, MUTED);
        if (notice) lv_label_set_text(record, notice);
        else lv_label_set_text_fmt(record, "最远纪录 %u m", (unsigned)game_best);
        lv_obj_t *start = rounded_box(body, 59, 200, 122, 29, 0xffb362, 8);
        lv_obj_center(label(start, "开始", 0, 0, &passport_game, INK));
        page_icons();
        return;
    }

    lv_obj_set_style_bg_color(body, color(0x211b39), 0);
    label(body, "喵喵快递", 14, 14, &passport_game, 0xffe3ad);
    game_score = label(body, "", 100, 14, &passport_game, 0xffe3ad);
    lv_obj_set_width(game_score, 124);
    lv_obj_set_style_text_align(game_score, LV_TEXT_ALIGN_RIGHT, 0);
    game_view = box(body, 0, 40, WIDTH, 212, BG);
    game_paint = game;
    meow_view_bind(game_view, &game_paint);
    game_hint = label(body, "", 16, 258, &passport_game, 0xffe3ad);
    game_totals = label(body, "", 16, 281, &passport_game, 0xe8abc2);
    lv_obj_set_width(game_totals, 208);
    lv_label_set_long_mode(game_totals, LV_LABEL_LONG_DOT);
    centered(body, "长按确认返回", 303, &passport_caption, 0xc9adc9);
    if (game_screen == GAME_PLAY) {
        update_game_view();
        return;
    }

    update_game_stats();
    if (game_screen == GAME_RESULT) {
        rounded_box(body, 20, 81, 200, 139, 0x302244, 10);
        centered(body, "配送结束", 92, &passport_game, 0xffe3ad);
        lv_obj_t *distance = centered(body, "", 115, &passport_game, TEXT);
        lv_label_set_text_fmt(distance, "跑了 %u m", (unsigned)meow_distance(&game));
        lv_obj_t *totals = centered(body, "", 138, &passport_game, TEXT);
        lv_label_set_text_fmt(totals, "鱼干 %u    送达 %u", (unsigned)game.fish, (unsigned)game.deliveries);
        lv_obj_t *record = centered(body, notice ? notice : "", 161, &passport_game, 0xe8abc2);
        if (!notice) lv_label_set_text_fmt(record, "最远纪录 %u m", (unsigned)game_best);
        centered(body, "确认重试", 188, &passport_game, 0xffb362);
    } else {
        bool paused = game_screen == GAME_PAUSED;
        rounded_box(body, 25, 108, 190, 73, 0x302244, 10);
        centered(body, paused ? "已暂停" : "麦克风不可用", 119, &passport_game, 0xffe3ad);
        centered(body, paused ? "确认继续" : "确认重试", 148, &passport_game, 0xffb362);
    }
}

static void rebuild(const web_status_t *web, passport_profile_t content, quota_snapshot_t quota, time_t now, int passkey)
{
    lv_obj_clean(body);
    city_title = NULL;
    photo_counter = NULL;
    city_render.samples = 0;
    photo_render_started = 0;
    image_view = NULL;
    image_detached = false;
    bool photo = preferences.page == PHOTO;
    bool city_page = preferences.page == CITY;
    bool playing = preferences.page == GAME && game_screen != GAME_COVER;
    bool full = photo || city_page || playing;
    lv_obj_set_style_bg_color(body, color(BG), 0);
    if (photo || playing) lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(body, full ? 0 : HEADER_HEIGHT);
    lv_obj_set_height(body, full ? HEIGHT : HEIGHT - HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, city_page ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(weekday_label, color(city_page ? TEXT : MUTED), 0);
    const uint8_t *pixels;
    switch (preferences.page) {
    case BADGE:
        displayed_badge = content.badge;
        pixels = asset_data(false, content.badge);
        if (pixels) {
            render_image_data(pixels, WIDTH, HEIGHT - HEADER_HEIGHT, BADGE_PIXELS, LV_COLOR_FORMAT_RGB565);
            // 112×176 版拥有固定右栏；更早的方形头像版保留原始整页布局。
            if (content.portrait) align_identity_text(pixels);
        }
        else {
            empty_state();
        }
        page_icons();
        break;
    case PHOTO: {
        uint8_t slot = selected_photo();
        displayed_photo = slot;
        pixels = asset_data(true, slot);
        size_t size = photo_asset_size(slot);
        if (pixels && size) {
            photo_render_bytes = size;
            photo_render_started = esp_timer_get_time();
            render_image_data(pixels, WIDTH, HEIGHT, size,
                              size == PHOTO_BYTES ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RAW);
            render_photo_counter();
        }
        else {
            rounded_box(body, 88, 108, 64, 64, SURFACE, 20);
            centered(body, LV_SYMBOL_IMAGE, 126, &lv_font_montserrat_28, ACCENT);
            centered(body, "添加照片", 194, &passport_heading, TEXT);
            centered(body, "长按确认进入设置", 226, &passport_caption, MUTED);
        }
        break;
    }
    case QUOTA: render_quota(quota, now); break;
    case CITY: {
        static const char *const names[PIXEL_SCENE_COUNT] = {
            [PIXEL_STREET] = "像\n素\n街\n角",
            [PIXEL_PARK] = "林\n间\n公\n园",
            [PIXEL_HARBOR] = "灯\n塔\n海\n湾",
        };
        int64_t frame_at = esp_timer_get_time();
        pixel_city_init(&city, body, preferences.scene);
        lv_obj_move_foreground(header);
        if (frame_at < city_title_until) {
            city_title = rounded_box(body, WIDTH - 43, HEADER_HEIGHT + 9, 27, 80, BG, 10);
            lv_obj_set_style_bg_opa(city_title, LV_OPA_90, 0);
            lv_obj_center(label(city_title, names[preferences.scene], 0, 0, &passport_caption, TEXT));
        }
        city_render.samples = 50;
        city_render.total_us = city_render.peak_us = 0;
        city_render.skip_first = true;
        next_city_frame = frame_at;
        update_city_frame(now, frame_at);
        break;
    }
    case GAME: render_game(); break;
    case SETTINGS: render_settings(web, passkey); break;
    }
}

static void init_screen(void)
{
    root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, color(BG), 0);
    header = box(root, 0, 0, WIDTH, HEADER_HEIGHT, BG);
    clock_label = label(header, "", 18, 0, &lv_font_montserrat_20, TEXT);
    weekday_label = label(header, "", 82, 0, &passport_caption, MUTED);
    wifi_icon = label(header, LV_SYMBOL_WIFI, 126, 0, &lv_font_montserrat_14, INACTIVE);
    bluetooth_icon = label(header, LV_SYMBOL_BLUETOOTH, 149, 0, &lv_font_montserrat_14, INACTIVE);
    battery_label = label(header, "", 166, 0, &lv_font_montserrat_14, TEXT);
    battery_icon = label(header, LV_SYMBOL_BATTERY_EMPTY, 208, 0, &lv_font_montserrat_14, TEXT);
    lv_obj_t *items[] = {clock_label, weekday_label, wifi_icon, bluetooth_icon, battery_label, battery_icon};
    for (size_t i = 0; i < sizeof items / sizeof items[0]; ++i) {
        const lv_font_t *font = lv_obj_get_style_text_font(items[i], 0);
        // 数字、汉字和图标按字体基线对齐，避免不同字框高度造成上下漂移。
        lv_obj_set_y(items[i], 26 - (font->line_height - font->base_line));
    }
    body = box(root, 0, HEADER_HEIGHT, WIDTH, HEIGHT - HEADER_HEIGHT, BG);
}

static esp_err_t persist(unsigned mask)
{
    esp_err_t err = update_preferences(mask, &preferences);
    preferences = preferences_snapshot();
    if (err != ESP_OK) {
        ESP_LOGE("passport", "Settings save: %s", esp_err_to_name(err));
        notice = "保存失败";
    }
    if ((mask & PREF_BRIGHTNESS) && !dark) bsp_display_backlight(preferences.brightness);
    if (mask & PREF_VOLUME) music_set_volume(preferences.volume);
    return err;
}

static void open_editor(int setting)
{
    notice = NULL;
    editing = setting;
    switch (setting) {
    case WIFI_POWER: edit_draft = network_enabled(); break;
    case BLE_POWER: edit_draft = quota_ble_enabled(); break;
    case TASK_CHIME_ENABLE: edit_draft = music_completion_enabled(); break;
    case TASK_CHIME_VOLUME:
        edit_saved = music_completion_volume();
        edit_draft = edit_saved == MUSIC_VOLUME_FOLLOW ? music_volume() : edit_saved;
        return;
    case BRIGHTNESS: edit_draft = preferences.brightness; break;
    case VOLUME: edit_draft = preferences.volume; break;
    case POWER_MODE: edit_draft = preferences.power_mode; break;
    case DEEP_SLEEP_SETTING:
        for (size_t i = 0; i < sizeof deep_sleep_options / sizeof deep_sleep_options[0]; ++i)
            if (deep_sleep_options[i] == preferences.deep_sleep_seconds) edit_draft = i;
        break;
    case MUSIC_ENABLED_SETTING: edit_draft = music_status().enabled; break;
    case MUSIC_SELECTION_SETTING: edit_draft = music_status().selection; break;
    case SLEEP:
        for (size_t i = 0; i < sizeof sleep_options / sizeof sleep_options[0]; ++i)
            if (sleep_options[i] == preferences.sleep_seconds) edit_draft = i;
        break;
    }
    edit_saved = edit_draft;
}

static esp_err_t save_editor(void)
{
    if (edit_draft == edit_saved) return ESP_OK;
    switch (editing) {
    case WIFI_POWER: return network_set_enabled(edit_draft);
    case BLE_POWER: return quota_ble_set_enabled(edit_draft);
    case TASK_CHIME_ENABLE: return music_set_completion_enabled(edit_draft);
    case TASK_CHIME_VOLUME: return music_set_completion_volume(edit_draft);
    case BRIGHTNESS:
        preferences.brightness = edit_draft;
        return persist(PREF_BRIGHTNESS);
    case VOLUME: {
        preferences.volume = edit_draft;
        esp_err_t err = persist(PREF_VOLUME);
        if (err == ESP_OK) music_volume_sample();
        return err;
    }
    case SLEEP:
        preferences.sleep_seconds = sleep_options[edit_draft];
        return persist(PREF_SLEEP);
    case DEEP_SLEEP_SETTING:
        preferences.deep_sleep_seconds = deep_sleep_options[edit_draft];
        return persist(PREF_DEEP_SLEEP);
    case POWER_MODE:
        preferences.power_mode = edit_draft;
        return persist(PREF_POWER);
    case MUSIC_ENABLED_SETTING: return music_toggle();
    case MUSIC_SELECTION_SETTING: return music_select(edit_draft);
    default: return ESP_ERR_INVALID_ARG;
    }
}

static void preview_selected_melody(void)
{
    // 试听由上下键切换触发，和保存、页面重绘分开。
    esp_err_t err = music_preview_selection(edit_draft);
    if (err == ESP_ERR_NOT_ALLOWED) notice = "当前已静音";
    else if (err != ESP_OK) {
        ESP_LOGE("passport", "Preview melody %d: %s", edit_draft, esp_err_to_name(err));
        notice = "音乐设置失败";
    }
}

static void activate_setting(int setting)
{
    open_editor(setting);
    // 二选一直接切换并保存；数值和多档设置留在草稿页，等待明确确认。
    if (setting == BRIGHTNESS || setting == VOLUME || setting == TASK_CHIME_VOLUME || edit_count(setting) != 2) return;
    edit_draft = !edit_saved;
    esp_err_t err = save_editor();
    editing = -1;
    if (err != ESP_OK) {
        ESP_LOGE("passport", "Toggle setting %d: %s", setting, esp_err_to_name(err));
        notice = "保存失败";
    }
}

static void close_detail(void)
{
    if (web_visible) web_close();
    if (pairing_visible) quota_end_pairing();
    music_stop();
    web_visible = pairing_visible = storage_visible = false;
    sound_panel = SOUND_NONE;
    bluetooth_visible = power_visible = about_visible = false;
    maintenance_action = 0;
    editing = -1;
    config_deadline = 0;
}

static void handle_sound_panel(uint8_t key)
{
    bool chime = sound_panel == SOUND_CHIME;
    int count = chime ? CHIME_MENU_COUNT : MUSIC_MENU_COUNT;
    if (key != BSP_BTN_OK) {
        music_menu = (music_menu + (key == BSP_BTN_UP ? count - 1 : 1)) % count;
        return;
    }

    esp_err_t err = ESP_OK;
    if (chime) {
        switch (music_menu) {
        case CHIME_ENABLE: activate_setting(TASK_CHIME_ENABLE); break;
        case CHIME_FOLLOW:
            err = music_set_completion_volume(music_completion_volume() == MUSIC_VOLUME_FOLLOW ?
                                              music_volume() : MUSIC_VOLUME_FOLLOW);
            break;
        case CHIME_LEVEL: open_editor(TASK_CHIME_VOLUME); break;
        case CHIME_PREVIEW: err = music_preview_completion(); break;
        }
    } else {
        if (music_menu == MUSIC_ENABLE) activate_setting(MUSIC_ENABLED_SETTING);
        else if (music_menu == MUSIC_MELODY) activate_setting(MUSIC_SELECTION_SETTING);
        else if (music_menu == MUSIC_PREVIEW) err = music_preview_current();
    }
    if (err != ESP_OK) notice = err == ESP_ERR_NOT_ALLOWED ? "当前已静音" : "音乐设置失败";
}

static void handle_button(uint8_t key, int64_t now)
{
    last_input = now;
    if (preferences.page != PHOTO || key != BSP_BTN_OK) photo_counter_until = 0;
    if (dark) {
        dark = power_display(true, preferences.brightness) != ESP_OK;
        return;
    }
    notice = NULL;
    if (preferences.page == GAME) {
        if (game_screen != GAME_COVER) {
            if (key == 3) {
                stop_game(GAME_COVER);
                save_game_record();
            } else if (key == BSP_BTN_OK) {
                if (game_screen == GAME_PLAY) stop_game(GAME_PAUSED);
                else start_game(game_screen == GAME_RESULT, now);
            }
            return;
        }
        if (key == BSP_BTN_OK) {
            start_game(true, now);
            return;
        }
    }
    if (key == 3) {
        dirty = true;
        // 长按统一进入设置，或从设置逐级返回。
        if (preferences.page == SETTINGS) {
            if (editing >= 0) {
                if (editing == MUSIC_SELECTION_SETTING) music_stop();
                editing = -1;
                return;
            }
            if (bluetooth_visible && maintenance_action) { maintenance_action = 0; return; }
            if (bluetooth_visible && pairing_visible) { quota_end_pairing(); pairing_visible = false; config_deadline = 0; return; }
            if (web_visible || pairing_visible || sound_panel || storage_visible || maintenance_action || bluetooth_visible || power_visible || about_visible) {
                close_detail();
                return;
            }
            if (settings_group) {
                selection = settings_group;
                settings_group = 0;
                return;
            }
            preferences.page = preferences.return_page;
        } else {
            music_stop();
            city_title_until = 0;
            preferences.return_page = preferences.page;
            preferences.page = SETTINGS;
        }
        persist(PREF_PAGE);
        return;
    }
    if (preferences.page != SETTINGS) {
        if (key == BSP_BTN_OK && preferences.page == CITY) {
            dirty = true;
            preferences.scene = (preferences.scene + 1) % PIXEL_SCENE_COUNT;
            city_title_until = persist(PREF_SCENE) == ESP_OK ? now + 2000000 : 0;
        } else if (key == BSP_BTN_OK && preferences.page == QUOTA) {
            dirty = true;
            preferences.quota_focus = !preferences.quota_focus;
            persist(PREF_QUOTA_VIEW);
        } else if (key == BSP_BTN_OK && preferences.page == PHOTO) {
            esp_err_t err = select_next_photo();
            if (err == ESP_OK) photo_counter_until = now + 1800000;
            else if (err != ESP_ERR_NOT_FOUND) ESP_LOGE("passport", "Save photo selection: %s", esp_err_to_name(err));
        } else if (key < BSP_BTN_OK) {
            dirty = true;
            music_stop();
            city_title_until = 0;
            const unsigned count = sizeof(display_pages) / sizeof(*display_pages);
            unsigned index = 0;
            while (display_pages[index] != preferences.page) ++index;
            preferences.page = display_pages[(index + (key == BSP_BTN_UP ? count - 1 : 1)) % count];
            persist(PREF_PAGE);
        }
        return;
    }
    dirty = true;
    if (editing >= 0) {
        if (key == BSP_BTN_OK) {
            esp_err_t err = save_editor();
            if (err == ESP_OK) {
                if (editing == MUSIC_SELECTION_SETTING) music_stop();
                editing = -1; notice = "已保存";
            }
            else { ESP_LOGE("passport", "Apply setting %d: %s", editing, esp_err_to_name(err)); notice = "保存失败"; }
        } else if (editing == BRIGHTNESS || editing == VOLUME || editing == TASK_CHIME_VOLUME) {
            int low = editing == BRIGHTNESS ? 10 : 0;
            edit_draft += key == BSP_BTN_UP ? 10 : -10;
            if (edit_draft < low) edit_draft = low;
            if (edit_draft > 100) edit_draft = 100;
        } else {
            int count = edit_count(editing);
            edit_draft = (edit_draft + (key == BSP_BTN_UP ? count - 1 : 1)) % count;
            if (editing == MUSIC_SELECTION_SETTING) preview_selected_melody();
        }
        return;
    }
    if (maintenance_action) {
        if (key != BSP_BTN_OK) confirm_selected = !confirm_selected;
        else if (!confirm_selected) { if (bluetooth_visible) maintenance_action = 0; else close_detail(); }
        else {
            uint8_t action = maintenance_action;
            esp_err_t err = schedule_maintenance(action);
            if (err == ESP_OK) { close_detail(); esp_restart(); }
            ESP_LOGE("passport", "Schedule maintenance: %s", esp_err_to_name(err));
            notice = "操作失败，请重试";
        }
        return;
    }
    if (sound_panel) {
        handle_sound_panel(key);
        return;
    }
    if (web_visible) {
        if (key == BSP_BTN_OK) return;
        if (web_status().hotspot) web_join_qr = !web_join_qr;
        else {
            esp_err_t err = web_open(true);
            web_join_qr = true;
            if (err != ESP_OK) { close_detail(); notice = "热点启动失败"; }
        }
        return;
    }
    if (pairing_visible || storage_visible || about_visible) {
        return;
    }
    if (bluetooth_visible) {
        if (key != BSP_BTN_OK) bluetooth_menu = (bluetooth_menu + (key == BSP_BTN_UP ? 2 : 1)) % 3;
        else if (bluetooth_menu == 0) activate_setting(BLE_POWER);
        else if (bluetooth_menu == 1) {
            if (!quota_ble_enabled()) notice = "请先开启蓝牙";
            else { quota_pair(); pairing_visible = true; config_deadline = now + 120000000; }
        } else { maintenance_action = MAINTENANCE_FORGET_MAC; confirm_selected = false; }
        return;
    }
    if (power_visible) {
        if (key != BSP_BTN_OK) power_menu = !power_menu;
        else activate_setting(power_menu ? DEEP_SLEEP_SETTING : POWER_MODE);
        return;
    }
    if (key != BSP_BTN_OK) {
        int count = settings_group ? settings_counts[settings_group - 1] : 4;
        selection = (selection + (key == BSP_BTN_UP ? count - 1 : 1)) % count;
        return;
    }

    if (!settings_group) {
        if (selection == 0) { storage_visible = true; return; }
        settings_group = selection;
        selection = 0;
        settings_scroll_y = 0;
        return;
    }
    uint8_t menu = settings_items[settings_group - 1][selection];

    switch (menu) {
    case WIFI_POWER:
    case BLE_POWER:
    case BRIGHTNESS:
    case VOLUME:
    case SLEEP:
        activate_setting(menu);
        break;
    case WEB_SETUP:
        if (web_open(false) == ESP_OK) {
            web_visible = true;
            web_join_qr = web_status().hotspot;
        } else notice = "网页设置启动失败";
        break;
    case PAIR:
        bluetooth_visible = true;
        bluetooth_menu = 0;
        break;
    case POWER_MODE:
        power_visible = true;
        power_menu = 0;
        break;
    case ABOUT:
        about_visible = true;
        break;
    case BOOT_MUSIC:
    case TASK_CHIME:
        sound_panel = menu == BOOT_MUSIC ? SOUND_BOOT : SOUND_CHIME;
        music_menu = MUSIC_ENABLE;
        break;
    case FORGET_MAC:
    case RESET:
        maintenance_action = menu == RESET ? MAINTENANCE_RESET : MAINTENANCE_FORGET_MAC;
        confirm_selected = false;
        break;
    }
}

static void button_event(bsp_btn_t button, bsp_btn_ev_t event, void *arg)
{
    // BSP callbacks are serialized by the button timer. React to a debounced
    // release immediately since the UI has no double-click actions.
    static bool held_long[BSP_BTN_COUNT];
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&input_lock);
    bool ignored = ignore_wake_key || now < ignore_keys_until;
    if (ignore_wake_key && event == BSP_BTN_RELEASE) {
        ignore_wake_key = false;
        ignore_keys_until = now + 300000;
    }
    portEXIT_CRITICAL(&input_lock);
    if (event == BSP_BTN_PRESS) {
        held_long[button] = false;
    } else if (event == BSP_BTN_LONG) {
        held_long[button] = true;
        if (!ignored && button == BSP_BTN_OK) {
            uint8_t key = 3;
            xQueueSend(buttons, &key, 0);
        }
    } else if (event == BSP_BTN_RELEASE && !ignored && !held_long[button]) {
        uint8_t key = button;
        xQueueSend(buttons, &key, 0);
    }
}

static void display_log(lv_log_level_t level, const char *message)
{
    // LVGL 默认未接日志输出；保留解码/绘制失败，便于定位偶发二维码白块。
    if (level == LV_LOG_LEVEL_WARN || level == LV_LOG_LEVEL_ERROR) ESP_LOGW("lvgl", "%s", message);
}

static void log_runtime_budget(bool input)
{
    static unsigned lowest = UINT_MAX;
    if (!input && lowest != UINT_MAX) return;
    // ESP-IDF 的高水位单位为字节；仅在新的最低值出现时输出，保留真实设备证据。
    unsigned remaining = uxTaskGetStackHighWaterMark(NULL);
    if (remaining < lowest) {
        lowest = remaining;
        ESP_LOGI("passport", "Main stack minimum=%u bytes, heap=%u largest=%u", remaining,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());
    preferences = preferences_snapshot();
    game_best = load_meow_best();
    // 首个无尽版在 35 米左右过完教学；旧纪录达到 36 米就已通过。
    game_tutorial_done = load_meow_tutorial() || game_best >= 36;
    power_init();
    ESP_ERROR_CHECK(bsp_display_init());
    lv_display_t *display = bsp_lvgl_init();
    assert(display);
    lv_log_register_print_cb(display_log);
    ESP_ERROR_CHECK(music_init(preferences.volume));
    buttons = xQueueCreate(8, sizeof(uint8_t));
    assert(buttons);
    ignore_wake_key = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
    ESP_ERROR_CHECK(bsp_button_init(button_event, NULL));
    if (ignore_wake_key && bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN) {
        portENTER_CRITICAL(&input_lock);
        ignore_wake_key = false;
        ignore_keys_until = esp_timer_get_time() + 300000;
        portEXIT_CRITICAL(&input_lock);
    }
    web_init();
    quota_ble_init();
    assert(bsp_lvgl_lock(0));
    ESP_ERROR_CHECK(photo_jpeg_install_decoder() ? ESP_OK : ESP_ERR_NOT_FOUND);
    init_screen();
    lv_display_add_event_cb(display, photo_render_ready, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(display, city_render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, city_render_event, LV_EVENT_RENDER_READY, NULL);
    web_status_t initial_web = web_status();
    update_header(&initial_web, time(NULL));
    rebuild(&initial_web, profile_snapshot(), quota_snapshot(), time(NULL), quota_passkey());
    // 默认白色 LVGL 屏幕及初始化过程始终灭背光，首帧传完后一次点亮。
    lv_refr_now(display);
    ESP_ERROR_CHECK(bsp_display_present(preferences.brightness));
    bsp_lvgl_unlock();
    last_input = esp_timer_get_time();
    time_t last_minute = -1;
    uint32_t last_quota = 0;
    int last_passkey = -2;
    bool last_connected = false, last_web_client = false, last_paired = false;
    boot_music_status_t last_music = music_status();
    bool boot_music_pending = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED;
    int64_t last_chime = 0;
    char last_url[sizeof ((web_status_t *)0)->url] = {0};
    for (;;) {
        uint8_t key;
        unsigned wait_ms = 200;
        if (preferences.page == CITY && !dark) {
            const int64_t remaining = next_city_frame - esp_timer_get_time();
            wait_ms = remaining > 0 ? (remaining + 999) / 1000 : 0;
        }
        if (preferences.page == GAME && game_screen == GAME_PLAY && !dark) {
            int64_t remaining = last_game_frame + GAME_FRAME_US - esp_timer_get_time();
            wait_ms = remaining > 0 ? (remaining + 999) / 1000 : 0;
        }
        bool pressed = xQueueReceive(buttons, &key, pdMS_TO_TICKS(wait_ms));
        int64_t now = esp_timer_get_time();
        passport_preferences_t updated = preferences_snapshot();
        bool brightness_changed = updated.brightness != preferences.brightness;
        bool volume_changed = updated.volume != preferences.volume;
        if (brightness_changed || volume_changed ||
            updated.sleep_seconds != preferences.sleep_seconds || updated.power_mode != preferences.power_mode ||
            updated.deep_sleep_seconds != preferences.deep_sleep_seconds) {
            if (brightness_changed && !dark) bsp_display_backlight(updated.brightness);
            if (volume_changed) music_set_volume(updated.volume);
            if (preferences.page == SETTINGS) dirty = true;
            last_input = now;
        }
        preferences = updated;
        if (pressed) handle_button(key, now);
        bool game_frame = preferences.page == GAME && !dark && tick_game(now);
        web_poll();
        log_runtime_budget(pressed);
        if (web_visible && web_expired()) { close_detail(); dirty = true; }
        if (config_deadline && now >= config_deadline) {
            quota_end_pairing(); pairing_visible = false; config_deadline = 0; dirty = true;
        }
        idle_action_t idle = power_idle_action(&preferences, (now - last_input) / 1000000,
                                                dark, web_visible || pairing_visible);
        if (idle != IDLE_KEEP_RUNNING) {
            if (game_screen == GAME_PLAY) stop_game(GAME_PAUSED);
            music_stop();
            esp_err_t err;
            if (idle == IDLE_DEEP_SLEEP) err = power_deep_sleep();
            else { err = power_display(false, preferences.brightness); dark = err == ESP_OK; }
            if (err != ESP_OK) {
                ESP_LOGW("passport", "Idle power transition: %s", esp_err_to_name(err));
                last_input = now;
            }
        }
        poll_battery(now);
        int64_t completion_until;
        sync_status_t status = quota_sync_status(&completion_until);
        // 息屏时蓝牙还能接收完成事件。以事件的到期时间去重，重绘页面不会重播。
        if (!boot_music_pending && now < completion_until && completion_until != last_chime) {
            last_chime = completion_until;
            if (game_screen != GAME_PLAY) music_completion_chime();
        }
        if (dark) continue;
        web_status_t web = web_status();
        if (web_visible && web.hotspot && web.client != last_web_client) {
            web_join_qr = !web.client;
            dirty = true;
        }
        last_web_client = web.client;
        quota_snapshot_t quota = quota_snapshot();
        static uint32_t last_resets;
        reset_snapshot_t reset_state = quota_resets();
        if (last_resets != reset_state.updated_at) {
            // 后台额度更新只重绘额度页，避免传图时反复销毁并分配二维码。
            if (preferences.page == QUOTA) dirty = true;
            last_resets = reset_state.updated_at;
        }
        time_t wall = time(NULL);
        unsigned signal = status.active | (status.quota_failed ? 256u : 0);
        if (preferences.page == QUOTA) {
            if (quota_fresh(quota.updated_at, wall, status)) signal |= 512u;
            if (quota_window_expired(quota.windows[0].reset_at, wall)) signal |= 1024u;
            if (quota_window_expired(quota.windows[1].reset_at, wall)) signal |= 2048u;
        }
        static unsigned last_signal;
        if (preferences.page == QUOTA && signal != last_signal) dirty = true;
        last_signal = signal;
        int passkey = quota_passkey();
        bool connected = quota_connected();
        boot_music_status_t music = music_status();
        if (preferences.page == SETTINGS && (last_music.enabled != music.enabled || last_music.selection != music.selection)) dirty = true;
        last_music = music;
        if (!bsp_lvgl_lock(100)) continue;
        passport_profile_t content = profile_snapshot();
        if (preferences.page != PHOTO) update_header(&web, wall);
        if (dirty || image_detached ||
            (preferences.page == BADGE && displayed_badge != content.badge) ||
            (preferences.page == PHOTO && displayed_photo != selected_photo()) ||
            (preferences.page == QUOTA && (last_quota != quota.updated_at ||
             last_minute != wall / 60 || last_connected != connected)) ||
            (web_visible && strcmp(last_url, web.url)) ||
            (bluetooth_visible && (last_connected != connected || last_paired != quota_paired())) ||
            (pairing_visible && (last_passkey != passkey || last_connected != connected))) {
            rebuild(&web, content, quota, wall, passkey);
            dirty = false;
        } else if (game_frame && game_screen == GAME_PLAY) {
            update_game_view();
        } else if (preferences.page == CITY) {
            update_city_frame(wall, esp_timer_get_time());
        }
        if (photo_counter && now >= photo_counter_until) {
            lv_obj_delete(photo_counter);
            photo_counter = NULL;
        }
        update_completion_toast(now, completion_until);
        bsp_lvgl_unlock();
        if (boot_music_pending) {
            boot_music_pending = false;
            if (game_screen != GAME_PLAY) music_boot();
        }
        last_quota = quota.updated_at;
        last_minute = wall / 60;
        last_passkey = passkey;
        last_connected = connected;
        last_paired = quota_paired();
        strcpy(last_url, web.url);
    }
}
