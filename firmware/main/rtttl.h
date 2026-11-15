#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTTTL_MAX_BYTES 1024
#define RTTTL_MAX_NOTES 96
#define RTTTL_MAX_DURATION_MS 20000

typedef struct {
    uint16_t hz;  // 0 表示休止符。
    uint16_t duration_ms;
} music_note_t;

typedef struct {
    uint16_t count;
    music_note_t notes[RTTTL_MAX_NOTES];
} music_score_t;

// 解析完整乐谱并限制总播放时长；失败时 out 的内容不可播放。
bool rtttl_parse(const char *text, size_t length, music_score_t *out);
