#include "passport.h"

bool preferences_merge(passport_preferences_t *out, const passport_preferences_t *current,
                       const passport_preferences_t *value, unsigned mask)
{
    if (!mask || (mask & ~(PREF_DEVICE | PREF_PAGE | PREF_SCENE | PREF_QUOTA_VIEW))) return false;
    *out = *current;
    if (mask & PREF_PAGE) { out->page = value->page; out->return_page = value->return_page; }
    if (mask & PREF_BRIGHTNESS) out->brightness = value->brightness;
    if (mask & PREF_VOLUME) out->volume = value->volume;
    if (mask & PREF_SLEEP) out->sleep_seconds = value->sleep_seconds;
    if (mask & PREF_POWER) out->power_mode = value->power_mode;
    if (mask & PREF_DEEP_SLEEP) out->deep_sleep_seconds = value->deep_sleep_seconds;
    if (mask & PREF_SCENE) out->scene = value->scene;
    if (mask & PREF_QUOTA_VIEW) out->quota_focus = value->quota_focus;
    bool sleep_valid = out->sleep_seconds == 0 || out->sleep_seconds == 30 || out->sleep_seconds == 60 ||
                       out->sleep_seconds == 180 || out->sleep_seconds == 300;
    bool deep_valid = out->deep_sleep_seconds == 300 || out->deep_sleep_seconds == 900 || out->deep_sleep_seconds == 1800;
    return deep_valid && out->scene < PIXEL_SCENE_COUNT &&
           out->page <= CITY && out->return_page <= CITY && out->return_page != SETTINGS &&
           out->brightness >= 10 && out->brightness <= 100 &&
           out->volume <= 100 && out->power_mode <= POWER_PORTABLE && sleep_valid;
}
