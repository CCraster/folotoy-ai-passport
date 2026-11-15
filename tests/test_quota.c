#include "passport.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint8_t packet[] = {1,7,3,0,104,211,13,107,0,241,83,101};
    reset_snapshot_t value;
    assert(decode_reset_packet(packet, sizeof packet, &value));
    assert(value.count == 3 && value.known && value.complete && value.updated_at == 1700000000);
    assert(value.expires_at == 1796068200);
    assert(!decode_reset_packet(packet, 11, &value));
    packet[0] = 2; assert(!decode_reset_packet(packet, 12, &value)); packet[0] = 1;
    packet[1] = 8; assert(!decode_reset_packet(packet, 12, &value));
    packet[1] = 0; assert(!decode_reset_packet(packet, 12, &value));
    packet[2] = 0; for (int i=4;i<8;++i) packet[i] = 0;
    assert(decode_reset_packet(packet, 12, &value) && !value.known);
    packet[1] = 5; assert(decode_reset_packet(packet, 12, &value) && value.known && value.count == 0);
    packet[1] = 3; assert(!decode_reset_packet(packet, 12, &value));
    packet[1] = 1; packet[2] = 4;
    assert(decode_reset_packet(packet, 12, &value) && !value.complete && !value.expires_at);
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    assert(!decode_reset_packet(packet, 12, &value));
    uint8_t status_packet[] = {1,7,2};
    sync_status_t status;
    assert(decode_sync_status(status_packet, sizeof status_packet, &status));
    assert(status.active == 2 && status.tasks_known && status.completed && status.quota_failed);
    status_packet[1] = 1;
    assert(!decode_sync_status(status_packet, sizeof status_packet, &status));
    status_packet[2] = 0;
    assert(decode_sync_status(status_packet, sizeof status_packet, &status) && !status.tasks_known);
    assert(!decode_sync_status(status_packet, 2, &status));
    puts("Quota protocol tests passed: version, flags, size, unknown/zero, missing expiry and timestamps.");
}
