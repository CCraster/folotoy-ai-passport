#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

// 地址和 AP IP 均保持网络字节序。只允许从 AP 接口进入的同网段 IPv4 客户端。
bool web_peer_is_hotspot(const struct sockaddr_storage *local, socklen_t local_size,
                         const struct sockaddr_storage *peer, socklen_t peer_size,
                         uint32_t ap_ip);
