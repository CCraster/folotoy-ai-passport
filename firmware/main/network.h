#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NETWORK_LIST_MAX 12
typedef enum {
    NETWORK_MESSAGE_NONE, NETWORK_SCAN_FAILED, NETWORK_CONNECTED_SAVED,
    NETWORK_SAVE_FAILED, NETWORK_CONNECT_FAILED,
} network_message_t;

typedef struct { char ssid[33]; int8_t signal; bool secure; } network_entry_t;
typedef struct {
    uint32_t ip, ap_ip;
    bool enabled, hotspot, client, scanning, connecting, error;
    char ssid[33], password[17], connected_ssid[33];
    network_message_t message;
    unsigned count;
    network_entry_t entries[NETWORK_LIST_MAX];
} network_status_t;

void network_init(void);
// 仅控制日常路由器连接；网页设置可临时使用热点，退出恢复该开关。
esp_err_t network_set_enabled(bool enabled);
network_status_t network_status(void);
// 只读开关时避免把完整扫描列表复制到调用栈。
bool network_enabled(void);
bool network_hotspot_active(void);
esp_err_t network_open(bool force_hotspot);
void network_close(void);
esp_err_t network_shutdown(void);
// HTTP 任务只排队；无线切换、连接验证与保存由 UI 任务的 poll 串行执行。
esp_err_t network_connect(const char *ssid, const char *password);
esp_err_t network_scan(void);
void network_poll(void);
