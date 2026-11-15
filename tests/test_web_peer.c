#include "web_peer.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct sockaddr_storage address(const char *text, bool mapped)
{
    struct sockaddr_storage result = {0};
    if (mapped) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&result;
        v6->sin6_family = AF_INET6;
        assert(inet_pton(AF_INET6, text, &v6->sin6_addr) == 1);
    } else {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&result;
        v4->sin_family = AF_INET;
        assert(inet_pton(AF_INET, text, &v4->sin_addr) == 1);
    }
    return result;
}

int main(void)
{
    uint32_t ap = inet_addr("192.168.4.1");
    struct sockaddr_storage local = address("192.168.4.1", false), peer = address("192.168.4.2", false);
    assert(web_peer_is_hotspot(&local, sizeof(struct sockaddr_in), &peer, sizeof(struct sockaddr_in), ap));
    local = address("::ffff:192.168.4.1", true);
    peer = address("::ffff:192.168.4.2", true);
    assert(web_peer_is_hotspot(&local, sizeof(struct sockaddr_in6), &peer, sizeof(struct sockaddr_in6), ap));
    assert(!web_peer_is_hotspot(&local, sizeof(struct sockaddr_in), &peer, sizeof peer, ap));
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof(struct sockaddr_in), ap));
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, 0));
    peer = address("::ffff:192.168.1.2", true);
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, ap));
    peer = address("192.168.4.2", false);
    local = address("192.168.1.30", false);
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, ap));
    local = address("::ffff:192.168.4.1", true);
    assert(web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, ap));
    peer = address("2001:db8::c0a8:402", true);
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, ap));
    peer.ss_family = AF_UNIX;
    assert(!web_peer_is_hotspot(&local, sizeof local, &peer, sizeof peer, ap));
    puts("Web peer tests passed: IPv4, mapped IPv6, truncated address and LAN isolation.");
}
