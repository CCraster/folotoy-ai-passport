#include <assert.h>
#include <stdio.h>

#include "meow_delivery.h"

static void detector_test(void)
{
    meow_detector_t detector = {0};
    int16_t silence[MEOW_PCM_SAMPLES], voiced[MEOW_PCM_SAMPLES];
    for (unsigned i = 0; i < MEOW_PCM_SAMPLES; ++i) {
        silence[i] = 400; // DC offset must not trigger.
        voiced[i] = 400 + (i % 2 ? 300 : -300);
    }
    meow_detect(&detector, silence);
    meow_detect(&detector, voiced);
    assert(detector.voice.ready && !detector.voice.onset);
    meow_detect(&detector, voiced);
    assert(detector.voice.onset == 1 && !detector.voice.stretch);
    for (int i = 2; i < 34; ++i) meow_detect(&detector, voiced);
    for (int i = 0; i < 3; ++i) meow_detect(&detector, silence);
    assert(!detector.voice.voiced && !detector.voice.stretch); // 340 ms is short.
    for (int i = 0; i < 34; ++i) meow_detect(&detector, voiced);
    assert(detector.voice.onset == 2 && !detector.voice.stretch);
    meow_detect(&detector, voiced);
    assert(detector.voice.stretch == 2); // Stretch belongs to the same onset.
    for (int i = 0; i < 150; ++i) meow_detect(&detector, voiced);
    assert(detector.voice.onset == 2);
}

static void approach_test(meow_kind_t kind, bool long_meow)
{
    meow_game_t game;
    meow_game_reset(&game, 42, true);
    game.track[MEOW_CURRENT] = (meow_obstacle_t){.start = 220, .kind = kind};
    game.x = 190 * MEOW_UNIT;
    meow_voice_t voice = {.onset = 1};
    bool fell = false, pulled = false;
    for (unsigned ms = 0; ms <= 2000 && game.track[MEOW_CURRENT].start == 220; ms += 10) {
        if (ms == 330 && long_meow) voice.stretch = 1;
        if (kind == MEOW_AWNING && !long_meow && voice.onset == 1 &&
            game.motion == MEOW_RUN && game.x >= 272 * MEOW_UNIT) {
            assert(game.fish == 2);
            ++voice.onset;
        }
        meow_game_step(&game, 10, voice);
        if (!ms) assert(game.motion == MEOW_JUMP && game.height > 0);
        if (ms == 330 && long_meow) assert(game.motion == MEOW_REACH);
        if (game.motion == MEOW_FALL) fell = true;
        if (game.motion == MEOW_PULL) pulled = true;
    }
    if (kind == MEOW_GAP && !long_meow) {
        assert(fell && game.motion == MEOW_RUN && game.track[MEOW_CURRENT].start == 220);
        assert(game.x < 220 * MEOW_UNIT && !game.rescue_used);
        return;
    }
    assert(!fell && game.track[MEOW_CURRENT].start != 220);
    assert(pulled == (kind != MEOW_BOX && long_meow));
    assert(game.fish == (long_meow ? 0u : kind == MEOW_AWNING ? 2u : 1u));
}

static void rescue_test(void)
{
    meow_game_t game;
    meow_game_reset(&game, 0, false);
    game.track[MEOW_CURRENT] = (meow_obstacle_t){.start = 5000, .kind = MEOW_AWNING};
    game.x = (5000 + MEOW_AWNING_RIGHT - 1) * MEOW_UNIT;
    game.farthest = game.x;
    game.fish = 2;
    game.track[MEOW_CURRENT].fish_taken = true;
    unsigned difficulty = meow_difficulty(&game);
    meow_game_step(&game, 620, (meow_voice_t){0});
    assert(game.motion == MEOW_RUN && game.rescue_used && game.fish == 2);
    assert(meow_difficulty(&game) == difficulty && game.farthest >= game.x);
    assert(game.x < 5000 * MEOW_UNIT);
    // 落回同一个雨棚不能重复捡鱼；第二次走空时结算。
    while (game.x < (5000 - 30) * MEOW_UNIT) meow_game_step(&game, 10, (meow_voice_t){0});
    meow_voice_t voice = {.onset = 1};
    meow_game_step(&game, 680, voice);
    assert(game.motion == MEOW_RUN && game.fish == 2);
    meow_game_step(&game, 1000, voice);
    assert(game.motion == MEOW_DONE && game.fish == 2);
}

static void endless_run(uint32_t seed, bool collect_awning)
{
    meow_game_t game;
    bool teach = seed % 4 < 2;
    meow_game_reset(&game, seed, teach);
    assert(game.teaching_left == (teach ? 2 : 0));
    if (teach) {
        assert(game.track[MEOW_CURRENT].start == 220 && game.track[MEOW_CURRENT].kind == MEOW_BOX);
        assert(game.track[MEOW_CURRENT + 1].start == 618 && game.track[MEOW_CURRENT + 1].kind == MEOW_GAP);
    } else {
        assert(game.track[MEOW_CURRENT].start < 220 && game.track[MEOW_CURRENT].kind != MEOW_NONE);
    }
    meow_voice_t voice = {0};
    int64_t started_at = -1;
    bool first_awn_jump = false;
    unsigned last_level = 0, kinds = 0, last_delivery_ms = 0;
    unsigned onset_ms = 0;
    bool stretch = false;
    for (unsigned ms = 0; ms < 390000; ms += 33) {
        const meow_obstacle_t *obstacle = &game.track[MEOW_CURRENT];
        meow_kind_t kind = obstacle->kind;
        int64_t x = game.x / MEOW_UNIT;
        int64_t front = kind == MEOW_CART ? meow_cart_position(obstacle, game.elapsed_ms) : obstacle->start;
        if (game.motion == MEOW_RUN && kind != MEOW_DELIVERY) {
            if (started_at != obstacle->start && x >= front - 32 + (int)(seed % 7)) {
                ++voice.onset;
                started_at = obstacle->start;
                onset_ms = ms;
                first_awn_jump = kind == MEOW_AWNING && collect_awning;
                stretch = kind == MEOW_GAP || (kind == MEOW_AWNING && !collect_awning);
                kinds |= 1u << kind;
            } else if (first_awn_jump && started_at == obstacle->start &&
                       x >= obstacle->start + MEOW_AWNING_RIGHT - 12) {
                ++voice.onset;
                onset_ms = ms;
                first_awn_jump = false;
            }
        }
        if (stretch && ms - onset_ms >= 331) voice.stretch = voice.onset;
        uint32_t before = game.deliveries;
        int64_t farthest = game.farthest;
        meow_game_step(&game, 33, voice);
        if (game.motion == MEOW_FALL) {
            fprintf(stderr, "seed=%u ms=%u kind=%d front=%lld x=%lld level=%u\n",
                    seed, ms, kind, (long long)front, (long long)x, last_level);
            assert(!"A generated route rejected the legal rhythm");
        }
        assert(game.motion != MEOW_DONE && game.farthest >= farthest);
        unsigned level = meow_difficulty(&game);
        assert(level >= last_level);
        last_level = level;
        for (unsigned i = MEOW_CURRENT + 1; i < MEOW_TRACK_SIZE; ++i)
            assert(game.track[i].start > meow_obstacle_end(&game.track[i - 1]));
        if (game.deliveries != before) {
            assert(game.deliveries == before + 1 && game.teaching_left == 0);
            if (teach && game.deliveries == 1) assert(ms < 15000);
            assert(ms - last_delivery_ms < 45000);
            last_delivery_ms = ms;
        }
    }
    assert(last_level == 4 && game.deliveries >= 12 && meow_distance(&game) >= 1400);
    assert(kinds == ((1u << MEOW_BOX) | (1u << MEOW_GAP) | (1u << MEOW_AWNING) | (1u << MEOW_CART)));
}

int main(void)
{
    detector_test();
    for (meow_kind_t kind = MEOW_BOX; kind <= MEOW_AWNING; ++kind) {
        approach_test(kind, false);
        approach_test(kind, true);
    }
    rescue_test();
    for (uint32_t seed = 0; seed < 32; ++seed)
        endless_run(seed, seed % 2 == 0);
    puts("Meow game passed: original voice timing, short/long choices, rescue and 32 endless runs of 6.5 minutes each.");
}
