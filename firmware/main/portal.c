#include "portal.h"
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

static int dns_socket = -1;
static uint32_t portal_address;
static atomic_bool stopping;
static SemaphoreHandle_t stopped;

static void serve_dns(void *arg)
{
    uint8_t query[512], reply[512];
    while (!atomic_load(&stopping)) {
        struct sockaddr_in peer;
        socklen_t size = sizeof peer;
        int count = recvfrom(dns_socket, query, sizeof query, 0, (struct sockaddr *)&peer, &size);
        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                ESP_LOGW("portal", "DNS receive: errno=%d", errno);
            continue;
        }
        size_t bytes = portal_dns_reply(query, count, reply, sizeof reply, portal_address);
        if (bytes) sendto(dns_socket, reply, bytes, 0, (struct sockaddr *)&peer, size);
    }
    close(dns_socket);
    dns_socket = -1;
    xSemaphoreGive(stopped);
    vTaskDelete(NULL);
}

esp_err_t portal_start(uint32_t address)
{
    if (dns_socket >= 0) return ESP_ERR_INVALID_STATE;
    if (!stopped) stopped = xSemaphoreCreateBinary();
    if (!stopped) return ESP_ERR_NO_MEM;
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = address};
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket < 0) return ESP_FAIL;
    struct timeval timeout = {.tv_usec = 200000};
    if (setsockopt(dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
        bind(dns_socket, (struct sockaddr *)&local, sizeof local)) {
        close(dns_socket); dns_socket = -1;
        return ESP_FAIL;
    }
    portal_address = address;
    atomic_store(&stopping, false);
    if (xTaskCreate(serve_dns, "portal_dns", 3072, NULL, 3, NULL) != pdPASS) {
        close(dns_socket); dns_socket = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void portal_stop(void)
{
    if (dns_socket < 0) return;
    atomic_store(&stopping, true);
    // recvfrom 有 200ms 超时；等任务释放 socket 后才允许重新开热点。
    xSemaphoreTake(stopped, portMAX_DELAY);
}
