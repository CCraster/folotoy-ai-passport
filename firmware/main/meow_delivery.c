#include "meow_delivery.h"

#include <stdlib.h>

void meow_detect(meow_detector_t *detector, const int16_t pcm[MEOW_PCM_SAMPLES])
{
    // 去直流后取平均振幅。ponytail: 首版识别有声/静音，环境噪声和人声也可能触发。
    enum { SOUND_ON = 260, SOUND_OFF = 165, RELEASE_FRAMES = 3 };
    int sum = 0;
    for (size_t i = 0; i < MEOW_PCM_SAMPLES; ++i) sum += pcm[i];
    int mean = sum / MEOW_PCM_SAMPLES;
    int amplitude = 0;
    for (size_t i = 0; i < MEOW_PCM_SAMPLES; ++i) amplitude += abs(pcm[i] - mean);
    meow_voice_t *voice = &detector->voice;
    bool audible = amplitude >= (voice->voiced ? SOUND_OFF : SOUND_ON) * MEOW_PCM_SAMPLES;
    if (audible) {
        detector->quiet_frames = 0;
        if (detector->held_ms < MEOW_LONG_MS) detector->held_ms += 10;
        if (!voice->voiced && detector->held_ms >= 20) {
            voice->voiced = true;
            ++voice->onset;
        }
        if (voice->voiced && detector->held_ms >= MEOW_LONG_MS) voice->stretch = voice->onset;
    } else if (!voice->voiced || ++detector->quiet_frames >= RELEASE_FRAMES) {
        voice->voiced = false;
        detector->held_ms = 0;
    }
    voice->ready = true;
}

enum {
    RUN_SPEED = 70, JUMP_SPEED = 100, JUMP_MS = 680, REACH_MS = 370,
    PULL_MS = 180, FALL_MS = 560, BOX_WIDTH = 18, GAP_WIDTH = 74, AWNING_WIDTH = 94,
};

int64_t meow_obstacle_end(const meow_obstacle_t *obstacle)
{
    switch (obstacle->kind) {
    case MEOW_BOX: return obstacle->start + BOX_WIDTH;
    case MEOW_GAP: return obstacle->start + GAP_WIDTH;
    case MEOW_AWNING: return obstacle->start + AWNING_WIDTH;
    case MEOW_CART: return obstacle->start + MEOW_CART_SWING + MEOW_CART_WIDTH;
    default: return obstacle->start;
    }
}

int64_t meow_cart_position(const meow_obstacle_t *obstacle, uint64_t elapsed_ms)
{
    // 同一节屋檐的推车有各自的起始相位，动画和碰撞共用同一个位置。
    // 推车远离玩家时，短跳仍可在落地前越过它。
    enum { HALF_CYCLE_MS = 2400 };
    unsigned phase = (elapsed_ms + (uint64_t)obstacle->start * 13) % (2 * HALF_CYCLE_MS);
    if (phase > HALF_CYCLE_MS) phase = 2 * HALF_CYCLE_MS - phase;
    return obstacle->start - MEOW_CART_SWING + 2 * MEOW_CART_SWING * (int)phase / HALF_CYCLE_MS;
}

unsigned meow_difficulty(const meow_game_t *game)
{
    int64_t distance = game->farthest / MEOW_UNIT;
    if (distance >= 15500) return 4;
    if (distance >= 9700) return 3;
    if (distance >= 4800) return 2;
    return distance >= 900;
}

uint32_t meow_distance(const meow_game_t *game)
{
    int64_t meters = game->farthest / (20 * MEOW_UNIT);
    return meters > UINT32_MAX ? UINT32_MAX : (uint32_t)meters;
}

static unsigned next_random(meow_game_t *game)
{
    game->random = game->random * 1664525u + 1013904223u;
    return game->random >> 16;
}

typedef struct {
    meow_kind_t obstacles[3];
} meow_pattern_t;

static const meow_kind_t single[] = {MEOW_BOX, MEOW_GAP, MEOW_AWNING, MEOW_CART};
static const meow_pattern_t combinations[] = {
    {{MEOW_CART, MEOW_AWNING}}, {{MEOW_AWNING, MEOW_GAP}}, {{MEOW_GAP, MEOW_CART}},
    {{MEOW_CART, MEOW_GAP, MEOW_BOX}}, {{MEOW_AWNING, MEOW_CART, MEOW_GAP}},
    {{MEOW_GAP, MEOW_AWNING, MEOW_CART}},
};

static meow_obstacle_t next_obstacle(meow_game_t *game, const meow_obstacle_t *previous)
{
    int64_t end = meow_obstacle_end(previous);
    unsigned level = meow_difficulty(game);
    // 组合内缩短间隔。组合间给玩家留一整段落地、收声和看下一障碍的时间。
    int padding = game->pattern_step ? 117 - 4 * (int)level : 176 - 6 * (int)level;
    if (previous->kind == MEOW_BOX) padding += MEOW_BOX_FISH + 10;
    if (previous->kind == MEOW_DELIVERY) padding = 176;
    if (!game->pattern_step && end - game->last_delivery >= 1680) {
        game->last_delivery = end + 105;
        return (meow_obstacle_t){.start = game->last_delivery, .kind = MEOW_DELIVERY};
    }

    meow_kind_t kind;
    if (game->introductions < 2) {
        kind = game->introductions++ == 0 ? MEOW_AWNING : MEOW_CART;
    } else if (game->pattern_step) {
        const meow_pattern_t *pattern = &combinations[game->pattern];
        kind = pattern->obstacles[game->pattern_step++];
        if (game->pattern_step == 3 || pattern->obstacles[game->pattern_step] == MEOW_NONE)
            game->pattern_step = 0;
    } else {
        unsigned odds = (const unsigned[]){0, 0, 42, 65, 84}[level];
        if (next_random(game) % 100 < odds) {
            // 先给双障碍，后续逐渐提高三障碍出现比例。
            bool triple = level >= 3 && next_random(game) % 100 < (level == 3 ? 30u : 65u);
            game->pattern = (triple ? 3 : 0) + next_random(game) % 3;
            game->pattern_step = 1;
            kind = combinations[game->pattern].obstacles[0];
        } else {
            unsigned type = next_random(game) % 4;
            if (single[type] == previous->kind) type = (type + 1) % 4;
            kind = single[type];
        }
    }
    if (kind == MEOW_CART) padding += MEOW_CART_SWING;
    return (meow_obstacle_t){.start = end + padding, .kind = kind};
}

void meow_game_reset(meow_game_t *game, uint32_t seed, bool teach)
{
    *game = (meow_game_t){.random = seed};
    if (teach) {
        game->track[1] = (meow_obstacle_t){.start = 220, .kind = MEOW_BOX};
        game->track[2] = (meow_obstacle_t){.start = 618, .kind = MEOW_GAP};
        game->track[3] = (meow_obstacle_t){.start = 884, .kind = MEOW_DELIVERY};
        game->last_delivery = 884;
        game->teaching_left = 2;
        return;
    }

    game->introductions = 2;
    for (unsigned i = MEOW_CURRENT; i < MEOW_TRACK_SIZE; ++i)
        game->track[i] = next_obstacle(game, &game->track[i - 1]);
}

static void pass_obstacle(meow_game_t *game)
{
    if (game->teaching_left) --game->teaching_left;
    for (unsigned i = 0; i + 1 < MEOW_TRACK_SIZE; ++i) game->track[i] = game->track[i + 1];
    game->track[MEOW_TRACK_SIZE - 1] = next_obstacle(game, &game->track[MEOW_TRACK_SIZE - 2]);
}

static void motion(meow_game_t *game, meow_motion_t next)
{
    game->motion = next;
    game->age = 0;
    game->from = game->x;
}

static void fail_obstacle(meow_game_t *game)
{
    game->retry_fall = game->teaching_left || !game->rescue_used;
    if (!game->teaching_left) game->rescue_used = true;
    game->fall_height = game->height;
    motion(game, MEOW_FALL);
}

static void advance_fall(meow_game_t *game, const meow_obstacle_t *obstacle)
{
    // 短暂扒住屋檐再滑下。救援点保留进度和鱼干，且位于上一个障碍之后。
    game->height = game->fall_height - (game->age > 150 ? (game->age - 150) / 5 : 0);
    if (game->age < FALL_MS) return;
    if (!game->retry_fall) {
        motion(game, MEOW_DONE);
        return;
    }
    int64_t checkpoint = obstacle->start - 94;
    int64_t previous_end = meow_obstacle_end(&game->track[0]);
    if (previous_end + 8 > checkpoint) checkpoint = previous_end + 8;
    game->x = checkpoint * MEOW_UNIT;
    game->height = 0;
    motion(game, MEOW_RUN);
}

static void take_fish(meow_game_t *game, meow_obstacle_t *obstacle, unsigned count)
{
    if (obstacle->fish_taken) return;
    obstacle->fish_taken = true;
    if (game->fish <= UINT32_MAX - count) game->fish += count;
}

static bool is_gap(meow_kind_t kind)
{
    return kind == MEOW_GAP || kind == MEOW_AWNING;
}

static void follow_course(meow_game_t *game, meow_obstacle_t *obstacle)
{
    int64_t start = obstacle->start * MEOW_UNIT;
    int64_t end = meow_obstacle_end(obstacle) * MEOW_UNIT;
    int64_t x = game->x;
    switch (obstacle->kind) {
    case MEOW_GAP:
    case MEOW_AWNING:
        if (x >= end && game->motion == MEOW_RUN) {
            pass_obstacle(game);
        } else if (x >= start && x < end && game->height == 0) {
            int64_t near = start + MEOW_AWNING_LEFT * MEOW_UNIT;
            int64_t far = start + MEOW_AWNING_RIGHT * MEOW_UNIT;
            if (obstacle->kind == MEOW_AWNING && x >= near && x < far) {
                take_fish(game, obstacle, 2);
            } else {
                // 从雨棚右边走空就抓雨棚边，普通跳短则抓起跳侧的屋檐。
                game->x = obstacle->kind == MEOW_AWNING && x >= far ? far : start;
                fail_obstacle(game);
            }
        }
        break;
    case MEOW_BOX: {
        int64_t pickup = end + MEOW_BOX_FISH * MEOW_UNIT;
        if (x >= start && x < end && game->height <= MEOW_BOX_HEIGHT) {
            fail_obstacle(game);
        } else if (x > pickup + 8 * MEOW_UNIT) {
            pass_obstacle(game);
        } else if (x >= pickup - 8 * MEOW_UNIT && game->height <= 8 && game->motion != MEOW_REACH) {
            take_fish(game, obstacle, 1);
        }
        break;
    }
    case MEOW_CART:
        start = meow_cart_position(obstacle, game->elapsed_ms) * MEOW_UNIT;
        end = start + MEOW_CART_WIDTH * MEOW_UNIT;
        if (x >= end) pass_obstacle(game);
        else if (x >= start && game->height <= MEOW_BOX_HEIGHT) fail_obstacle(game);
        break;
    case MEOW_DELIVERY:
        if (x >= start) {
            if (game->deliveries < UINT32_MAX) ++game->deliveries;
            pass_obstacle(game);
        }
        break;
    case MEOW_NONE:
        break;
    }
}

static void advance(meow_game_t *game, int ms)
{
    game->elapsed_ms += (unsigned)ms;
    game->age += ms;
    meow_obstacle_t *obstacle = &game->track[MEOW_CURRENT];
    switch (game->motion) {
    case MEOW_RUN:
        game->x += (RUN_SPEED + 5 * (int)meow_difficulty(game)) * ms;
        break;
    case MEOW_JUMP: {
        int t = game->age < JUMP_MS ? game->age : JUMP_MS;
        game->x += JUMP_SPEED * ms;
        game->height = 4 * 48 * t * (JUMP_MS - t) / (JUMP_MS * JUMP_MS);
        if (game->age >= JUMP_MS) motion(game, MEOW_RUN);
        break;
    }
    case MEOW_REACH: {
        int t = game->age < REACH_MS ? game->age : REACH_MS;
        game->x = game->from + (game->target - game->from) * t / REACH_MS;
        game->height = (game->fall_height * (REACH_MS - t) + REACH_MS - 1) / REACH_MS;
        if (game->age >= REACH_MS) {
            bool caught = is_gap(obstacle->kind) && game->target == meow_obstacle_end(obstacle) * MEOW_UNIT;
            motion(game, caught ? MEOW_PULL : MEOW_RUN);
        }
        break;
    }
    case MEOW_PULL:
        game->x += 45 * ms;
        if (game->age >= PULL_MS) motion(game, MEOW_RUN);
        break;
    case MEOW_FALL:
        advance_fall(game, obstacle);
        return;
    case MEOW_DONE:
        return;
    }

    follow_course(game, obstacle);
    if (game->x > game->farthest) game->farthest = game->x;
}

void meow_game_step(meow_game_t *game, unsigned ms, meow_voice_t voice)
{
    if (voice.onset != game->seen_onset) {
        game->seen_onset = voice.onset;
        if (game->motion == MEOW_RUN) {
            game->jump_voice = voice.onset;
            motion(game, MEOW_JUMP);
        }
    }
    if (game->motion == MEOW_JUMP && game->jump_voice && voice.stretch == game->jump_voice) {
        const meow_obstacle_t *obstacle = &game->track[MEOW_CURRENT];
        game->target = game->from + 132 * MEOW_UNIT;
        if (is_gap(obstacle->kind)) {
            int64_t end = meow_obstacle_end(obstacle) * MEOW_UNIT;
            if (game->x < end && game->target >= end) game->target = end;
        }
        game->fall_height = game->height;
        motion(game, MEOW_REACH);
    }
    // 每步不超过 10 ms，避免刷新卡顿时跨过碰撞或捡鱼判定。
    while (ms && game->motion != MEOW_DONE) {
        unsigned step = ms > 10 ? 10 : ms;
        advance(game, step);
        ms -= step;
    }
}
