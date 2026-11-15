#include "passport.h"

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool decode_reset_packet(const uint8_t *p, size_t length, reset_snapshot_t *out)
{
    if (length != 12 || p[0] != 1 || (p[1] & ~7)) return false;
    reset_snapshot_t value = {
        .known = (p[1] & 1) != 0, .complete = (p[1] & 4) != 0,
        .count = (uint16_t)p[2] | (uint16_t)p[3] << 8,
        .expires_at = little32(p + 4), .updated_at = little32(p + 8),
    };
    if (value.updated_at < 1700000000 || value.updated_at > 4102444800UL) return false;
    if (!value.known && (value.count || value.expires_at || value.complete)) return false;
    if ((bool)(p[1] & 2) != (value.expires_at != 0)) return false;
    if (value.expires_at && (!value.count || value.expires_at < 1700000000 || value.expires_at > 4102444800UL)) return false;
    *out = value;
    return true;
}


bool decode_sync_status(const uint8_t *p, size_t length, sync_status_t *out)
{
    if (length != SYNC_STATUS_BYTES || p[0] != 1 || (p[1] & ~7)) return false;
    sync_status_t value = {
        .active = p[2], .quota_failed = (p[1] & 1) != 0,
        .tasks_known = (p[1] & 2) != 0, .completed = (p[1] & 4) != 0,
    };
    if (!value.tasks_known && (value.active || value.completed)) return false;
    *out = value;
    return true;
}
