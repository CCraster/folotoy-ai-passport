#include "web_peer.h"
#include <string.h>

static bool ipv4(const struct sockaddr_storage *address, socklen_t size, uint32_t *ip)
{
    if (size < sizeof(struct sockaddr)) return false;
    if (address->ss_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
        *ip = ((const struct sockaddr_in *)address)->sin_addr.s_addr;
        return true;
    }
    if (address->ss_family == AF_INET6 && size >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        static const uint8_t mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        // IDF 双栈 HTTP socket 将 IPv4 连接返回为 ::ffff:a.b.c.d。
        if (!memcmp(v6->sin6_addr.s6_addr, mapped_prefix, sizeof mapped_prefix)) {
            memcpy(ip, v6->sin6_addr.s6_addr + 12, sizeof *ip);
            return true;
        }
    }
    return false;
}

bool web_peer_is_hotspot(const struct sockaddr_storage *local, socklen_t local_size,
                         const struct sockaddr_storage *peer, socklen_t peer_size,
                         uint32_t ap_ip)
{
    uint32_t local_ip, peer_ip;
    const uint32_t mask = htonl(0xffffff00);
    return ap_ip && ipv4(local, local_size, &local_ip) && ipv4(peer, peer_size, &peer_ip) &&
        local_ip == ap_ip && (peer_ip & mask) == (ap_ip & mask);
}
