#include "rtttl.h"

#include <ctype.h>
#include <string.h>

static void spaces(const char **p, const char *end)
{
    while (*p < end && isspace((unsigned char)**p)) ++*p;
}

static bool number(const char **p, const char *end, unsigned *out)
{
    unsigned value = 0;
    const char *start = *p;
    while (*p < end && isdigit((unsigned char)**p)) {
        value = value * 10 + (*(*p)++ - '0');
        if (value > 10000) return false;
    }
    *out = value;
    return *p != start;
}

static bool duration_valid(unsigned value)
{
    return value == 1 || value == 2 || value == 4 || value == 8 || value == 16 || value == 32;
}

bool rtttl_parse(const char *text, size_t length, music_score_t *out)
{
    if (!text || !out || !length || length > RTTTL_MAX_BYTES || memchr(text, 0, length)) return false;
    const char *end = text + length, *p = text;
    spaces(&p, end);
    const char *name = p;
    while (p < end && *p != ':') {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return false;
        ++p;
    }
    if (p == end || p == name || p - name > 64) return false;
    ++p;
    unsigned duration = 4, octave = 6, bpm = 63, fields = 0;
    spaces(&p, end);
    while (p < end && *p != ':') {
        char key = tolower((unsigned char)*p++);
        unsigned bit = key == 'd' ? 1 : key == 'o' ? 2 : key == 'b' ? 4 : 0;
        spaces(&p, end);
        if (!bit || (fields & bit) || p == end || *p++ != '=') return false;
        fields |= bit;
        spaces(&p, end);
        unsigned value;
        if (!number(&p, end, &value)) return false;
        if (key == 'd') { if (!duration_valid(value)) return false; duration = value; }
        if (key == 'o') { if (value < 4 || value > 7) return false; octave = value; }
        if (key == 'b') { if (value < 25 || value > 900) return false; bpm = value; }
        spaces(&p, end);
        if (p < end && *p == ':') break;
        if (p == end || *p++ != ',') return false;
        spaces(&p, end);
        if (p == end || *p == ':') return false;
    }
    if (p == end || *p++ != ':') return false;
    out->count = 0;
    unsigned total_ms = 0;
    // 四个八度的频率表，避免实时音频循环中的浮点指数运算。
    static const uint16_t frequencies[4][12] = {
        {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494},
        {523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988},
        {1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976},
        {2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951},
    };
    for (;;) {
        spaces(&p, end);
        if (p == end || out->count == RTTTL_MAX_NOTES) return false;
        unsigned d = duration, o = octave;
        if (isdigit((unsigned char)*p) && (!number(&p, end, &d) || !duration_valid(d))) return false;
        if (p == end) return false;
        char note = tolower((unsigned char)*p++);
        int semitone;
        switch (note) {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        case 'p': semitone = -1; break;
        default: return false;
        }
        if (p < end && *p == '#') {
            if (semitone < 0 || note == 'e' || note == 'b') return false;
            ++semitone;
            ++p;
        }
        bool dotted = p < end && *p == '.';
        if (dotted) ++p;
        if (p < end && isdigit((unsigned char)*p)) {
            if (semitone < 0 || !number(&p, end, &o) || o < 4 || o > 7) return false;
        }
        if (p < end && *p == '.') {
            if (dotted) return false;
            dotted = true;
            ++p;
        }
        unsigned ms = 240000 / (bpm * d);
        if (dotted) ms = ms * 3 / 2;
        total_ms += ms;
        if (total_ms > RTTTL_MAX_DURATION_MS) return false;
        out->notes[out->count++] = (music_note_t){semitone < 0 ? 0 : frequencies[o - 4][semitone], ms};
        spaces(&p, end);
        if (p == end) return true;
        if (*p++ != ',') return false;
    }
}
