#include "network.h"
#include "passport.h"
#include "portal.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "dhcpserver/dhcpserver.h"
#include "freertos/FreeRTOS.h"

static esp_netif_t *access_point;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static network_status_t state;
static bool station_started, trial, retry_requested, failed;
static unsigned job; // 1=扫描，2=连接；与 connecting/scanning 在同一临界区发布。
static char candidate_ssid[33], candidate_password[65];
static int64_t connect_deadline;

static void message(const char *text, bool error)
{
    portENTER_CRITICAL(&lock);
    snprintf(state.message, sizeof state.message, "%s", text);
    state.error = error;
    portEXIT_CRITICAL(&lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_ap_record_t ap = {0};
        // 丢弃接口已断开后排队到达的旧 IP 事件。
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
        portENTER_CRITICAL(&lock);
        bool accepted = station_started && (!trial || !strcmp((char *)ap.ssid, candidate_ssid));
        if (accepted) {
            state.ip = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
            snprintf(state.connected_ssid, sizeof state.connected_ssid, "%s", (char *)ap.ssid);
        }
        portEXIT_CRITICAL(&lock);
        if (accepted) esp_netif_sntp_start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        portENTER_CRITICAL(&lock);
        state.ip = 0;
        state.connected_ssid[0] = 0;
        if (trial && reason != WIFI_REASON_ASSOC_LEAVE) failed = true;
        if (station_started && !trial) retry_requested = true;
        portEXIT_CRITICAL(&lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_ap_record_t records[NETWORK_LIST_MAX];
        uint16_t count = NETWORK_LIST_MAX;
        esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
        portENTER_CRITICAL(&lock);
        state.count = 0;
        if (err == ESP_OK) for (unsigned i = 0; i < count; ++i) {
            if (!records[i].ssid[0]) continue;
            bool duplicate = false;
            for (unsigned j = 0; j < state.count; ++j)
                if (!strcmp(state.entries[j].ssid, (char *)records[i].ssid)) duplicate = true;
            if (duplicate) continue;
            network_entry_t *entry = &state.entries[state.count++];
            snprintf(entry->ssid, sizeof entry->ssid, "%s", (char *)records[i].ssid);
            entry->signal = records[i].rssi;
            entry->secure = records[i].authmode != WIFI_AUTH_OPEN;
        }
        state.scanning = false;
        portEXIT_CRITICAL(&lock);
        if (err != ESP_OK) message("扫描失败，请重试", true);
    }
}

static esp_err_t stop_station(void)
{
    portENTER_CRITICAL(&lock);
    bool was_started = station_started;
    station_started = retry_requested = false;
    portEXIT_CRITICAL(&lock);
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) err = ESP_OK;
    portENTER_CRITICAL(&lock);
    if (err == ESP_OK) {
        state.ip = 0;
        state.connected_ssid[0] = 0;
    } else station_started = was_started;
    portEXIT_CRITICAL(&lock);
    if (err != ESP_OK) ESP_LOGE("network", "Stop Wi-Fi radio: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t start_saved(void)
{
    if (!network_enabled()) return ESP_OK;
    char ssid[33], password[65];
    esp_err_t err = load_wifi(ssid, password);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE("network", "Read saved Wi-Fi: %s", esp_err_to_name(err));
        return err;
    }
    if (!ssid[0]) return ESP_OK;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&lock);
        station_started = true;
        portEXIT_CRITICAL(&lock);
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW("network", "Reconnect saved network: %s", esp_err_to_name(err));
        stop_station();
    }
    return err;
}

void network_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *station = esp_netif_create_default_wifi_sta();
    assert(station);
    access_point = esp_netif_create_default_wifi_ap();
    assert(access_point);
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    bool enabled;
    ESP_ERROR_CHECK(load_wifi_enabled(&enabled));
    state.enabled = enabled;
    start_saved();
}

network_status_t network_status(void)
{
    portENTER_CRITICAL(&lock);
    network_status_t result = state;
    portEXIT_CRITICAL(&lock);
    if (result.hotspot) {
        wifi_sta_list_t clients;
        result.client = esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0;
    }
    return result;
}

bool network_enabled(void)
{
    portENTER_CRITICAL(&lock);
    bool enabled = state.enabled;
    portEXIT_CRITICAL(&lock);
    return enabled;
}

bool network_hotspot_active(void)
{
    portENTER_CRITICAL(&lock);
    bool hotspot = state.hotspot;
    portEXIT_CRITICAL(&lock);
    return hotspot;
}

esp_err_t network_set_enabled(bool enabled)
{
    // 机身连接菜单调用，正在配置的热点由网页生命周期单独管理。
    if (network_hotspot_active()) return ESP_ERR_INVALID_STATE;
    bool previous = network_enabled();
    if (previous == enabled) return ESP_OK;
    esp_err_t err = stop_station();
    if (err != ESP_OK) return err;
    err = save_wifi_enabled(enabled);
    if (err != ESP_OK) { start_saved(); return err; }
    portENTER_CRITICAL(&lock);
    state.enabled = enabled;
    portEXIT_CRITICAL(&lock);
    if (enabled) err = start_saved();
    if (err != ESP_OK) {
        // 驱动未启动成功时恢复旧开关；保留凭据，由用户重试。
        esp_err_t restored = save_wifi_enabled(previous);
        if (restored == ESP_OK) {
            portENTER_CRITICAL(&lock);
            state.enabled = previous;
            portEXIT_CRITICAL(&lock);
        }
        ESP_LOGE("network", "Enable Wi-Fi: %s; restore setting: %s",
                 esp_err_to_name(err), esp_err_to_name(restored));
        return err;
    }
    ESP_LOGI("network", "Daily Wi-Fi %s", enabled ? "enabled" : "disabled");
    return err;
}

static void letters(char *out, size_t length)
{
    const char alphabet[] = "abcdefghjkmnpqrstuvwxyz";
    for (size_t i = 0; i < length; ++i) out[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
    out[length] = 0;
}

esp_err_t network_open(bool force_hotspot)
{
    portENTER_CRITICAL(&lock);
    bool ready = state.hotspot || (!force_hotspot && state.ip);
    portEXIT_CRITICAL(&lock);
    if (ready) return ESP_OK;
    esp_err_t err = stop_station();
    if (err != ESP_OK) return err;
    wifi_config_t ap = {0};
    char ssid[33] = "passport", password[17];
    letters(ssid + 8, 4);
    letters(password, 8);
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    memcpy(ap.ap.password, password, strlen(password));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    esp_netif_ip_info_t info;
    if (err == ESP_OK) err = esp_netif_get_ip_info(access_point, &info);
    if (err != ESP_OK) { stop_station(); start_saved(); return err; }
    esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
    dns.ip.u_addr.ip4 = info.ip;
    err = esp_netif_dhcps_stop(access_point);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) err = ESP_OK;
    if (err == ESP_OK) err = esp_netif_set_dns_info(access_point, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = OFFER_DNS;
    if (err == ESP_OK) err = esp_netif_dhcps_option(access_point, ESP_NETIF_OP_SET,
                                ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns);
    if (err != ESP_OK) ESP_LOGW("network", "Portal DNS advertisement: %s", esp_err_to_name(err));
    esp_err_t resumed = esp_netif_dhcps_start(access_point);
    if (resumed != ESP_OK && resumed != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        stop_station(); start_saved(); return resumed;
    }
    portENTER_CRITICAL(&lock);
    state.hotspot = true;
    state.ap_ip = info.ip.addr;
    snprintf(state.ssid, sizeof state.ssid, "%s", ssid);
    snprintf(state.password, sizeof state.password, "%s", password);
    state.message[0] = 0; state.error = false;
    portEXIT_CRITICAL(&lock);
    // 自动弹页失败时仍能用屏幕上的短网址；DNS 不承担设置数据写入。
    err = portal_start(info.ip.addr);
    if (err != ESP_OK) ESP_LOGW("network", "Portal DNS unavailable: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t network_scan(void)
{
    portENTER_CRITICAL(&lock);
    bool busy = !state.hotspot || state.connecting || state.scanning;
    if (!busy) { state.scanning = true; state.message[0] = 0; state.error = false; job = 1; }
    portEXIT_CRITICAL(&lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t network_connect(const char *ssid, const char *password)
{
    portENTER_CRITICAL(&lock);
    bool busy = !state.hotspot || state.connecting || state.scanning;
    if (!busy) {
        snprintf(candidate_ssid, sizeof candidate_ssid, "%s", ssid);
        snprintf(candidate_password, sizeof candidate_password, "%s", password);
        state.connecting = trial = true;
        failed = retry_requested = false;
        state.message[0] = 0; state.error = false;
        job = 2;
    }
    portEXIT_CRITICAL(&lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void network_poll(void)
{
    portENTER_CRITICAL(&lock);
    unsigned next = job;
    job = 0;
    bool reconnect = retry_requested;
    retry_requested = false;
    portEXIT_CRITICAL(&lock);
    if (next == 1) {
        esp_err_t err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&lock); state.scanning = false; portEXIT_CRITICAL(&lock);
            message("扫描失败，请重试", true);
        }
    } else if (next == 2) {
        esp_wifi_disconnect();
        portENTER_CRITICAL(&lock);
        state.ip = 0;
        state.connected_ssid[0] = 0;
        failed = false;
        station_started = true;
        portEXIT_CRITICAL(&lock);
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, candidate_ssid, strlen(candidate_ssid));
        memcpy(config.sta.password, candidate_password, strlen(candidate_password));
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) err = esp_wifi_connect();
        connect_deadline = esp_timer_get_time() + 25000000;
        if (err != ESP_OK) { portENTER_CRITICAL(&lock); failed = true; portEXIT_CRITICAL(&lock); }
    } else if (reconnect) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW("network", "Reconnect: %s", esp_err_to_name(err));
    }
    portENTER_CRITICAL(&lock);
    bool success = state.connecting && connect_deadline && state.ip;
    bool failure = state.connecting && connect_deadline && (failed || esp_timer_get_time() >= connect_deadline);
    portEXIT_CRITICAL(&lock);
    if (!success && !failure) return;
    // 取得 IP 后才覆盖已保存的网络，密码错误/取消保留原配置；热点保持供编辑和重试。
    esp_err_t saved = success ? save_wifi(candidate_ssid, candidate_password) : ESP_FAIL;
    portENTER_CRITICAL(&lock);
    station_started = success && saved == ESP_OK;
    trial = state.connecting = failed = false;
    connect_deadline = 0;
    memset(candidate_password, 0, sizeof candidate_password);
    portEXIT_CRITICAL(&lock);
    if (success && saved == ESP_OK) message("已连接并保存，可继续编辑", false);
    else {
        esp_wifi_disconnect();
        message(success ? "保存失败，原网络已保留" : "连接失败，请检查密码和网络后重试", true);
    }
}

void network_close(void)
{
    portal_stop();
    bool hotspot = network_hotspot_active();
    if (!hotspot) return;
    esp_wifi_scan_stop();
    portENTER_CRITICAL(&lock);
    trial = false;
    job = 0;
    connect_deadline = 0;
    state.hotspot = state.connecting = state.scanning = false;
    memset(candidate_password, 0, sizeof candidate_password);
    portEXIT_CRITICAL(&lock);
    stop_station();
    start_saved();
}

esp_err_t network_shutdown(void)
{
    network_close();
    esp_netif_sntp_deinit();
    stop_station();
    return esp_wifi_deinit();
}
