#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEOW_PCM_SAMPLES = 160, // 16 kHz, 10 ms
    MEOW_LONG_MS = 350,
    MEOW_UNIT = 1000, // 物理坐标为千分之一像素。
    MEOW_TRACK_SIZE = 4, // 上一个障碍、当前障碍和前方两个障碍。
    MEOW_CURRENT = 1,
    MEOW_BOX_FISH = 29,
    MEOW_BOX_HEIGHT = 15,
    MEOW_CART_WIDTH = 16,
    MEOW_CART_SWING = 18,
    MEOW_AWNING_LEFT = 18,
    MEOW_AWNING_RIGHT = 66,
};

typedef struct {
    uint32_t onset, stretch; // 同一次发声的起跳、变招共享编号。
    bool ready, failed, voiced;
} meow_voice_t;

typedef struct {
    meow_voice_t voice;
    uint16_t held_ms;
    uint8_t quiet_frames;
} meow_detector_t;

void meow_detect(meow_detector_t *detector, const int16_t pcm[MEOW_PCM_SAMPLES]);

typedef enum { MEOW_NONE, MEOW_BOX, MEOW_GAP, MEOW_AWNING, MEOW_CART, MEOW_DELIVERY } meow_kind_t;

typedef struct {
    int64_t start; // 世界坐标，单位像素。小推车以该点为移动中心。
    meow_kind_t kind;
    bool fish_taken;
} meow_obstacle_t;

typedef enum { MEOW_RUN, MEOW_JUMP, MEOW_REACH, MEOW_PULL, MEOW_FALL, MEOW_DONE } meow_motion_t;

typedef struct {
    meow_obstacle_t track[MEOW_TRACK_SIZE];
    int64_t x, farthest, from, target;
    int64_t last_delivery;
    uint64_t elapsed_ms;
    int height, age, fall_height;
    uint32_t seen_onset, jump_voice, random;
    uint32_t fish, deliveries;
    uint8_t teaching_left, introductions, pattern, pattern_step;
    bool rescue_used, retry_fall;
    meow_motion_t motion;
} meow_game_t;

void meow_game_reset(meow_game_t *game, uint32_t seed, bool teach);
void meow_game_step(meow_game_t *game, unsigned ms, meow_voice_t voice);
unsigned meow_difficulty(const meow_game_t *game);
uint32_t meow_distance(const meow_game_t *game);
int64_t meow_obstacle_end(const meow_obstacle_t *obstacle);
int64_t meow_cart_position(const meow_obstacle_t *obstacle, uint64_t elapsed_ms);
