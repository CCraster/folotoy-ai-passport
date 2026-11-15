#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// 仅在卡片热点地址监听 DNS，局域网客户端继续使用自己的 DNS。
esp_err_t portal_start(uint32_t address);
void portal_stop(void);

// 外部 DNS 报文边界：只接收一个未压缩的问题；IPv4 地址按网络字节序传入。
size_t portal_dns_reply(const uint8_t *query, size_t length, uint8_t *reply,
                        size_t capacity, uint32_t address);
