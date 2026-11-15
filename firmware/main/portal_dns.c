#include "portal.h"
#include <string.h>

size_t portal_dns_reply(const uint8_t *query, size_t length, uint8_t *reply,
                        size_t capacity, uint32_t address)
{
    if (length < 17 || (query[2] & 0xfa) || query[4] || query[5] != 1) return 0;
    size_t end = 12;
    while (end < length && query[end]) {
        size_t label = query[end++];
        if (label > 63 || label > length - end) return 0;
        end += label;
        if (end - 12 > 254) return 0;
    }
    if (end >= length || length - ++end < 4) return 0;
    bool ipv4 = query[end] == 0 && query[end + 1] == 1;
    if (query[end + 2] || query[end + 3] != 1) return 0;
    end += 4;
    size_t size = end + (ipv4 ? 16 : 0);
    if (size > capacity) return 0;
    memcpy(reply, query, end);
    reply[2] = 0x84 | (query[2] & 1);
    reply[3] = 0;
    memset(reply + 6, 0, 6);
    reply[7] = ipv4 ? 1 : 0;
    if (ipv4) {
        const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        memcpy(reply + end, answer, sizeof answer);
        memcpy(reply + end + sizeof answer, &address, 4);
    }
    return size;
}
