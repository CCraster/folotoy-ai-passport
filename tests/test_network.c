#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/network.c"

static int64_t now = 1;
static int mode, saves, connects;
static bool save_failure, scan_failure, stored_enabled = true, radio_running;
static esp_err_t start_error, connect_error, stop_error, ap_error, load_error;
static wifi_config_t current;
static char stored_ssid[33] = "old-home", stored_password[65] = "old-password";
static esp_netif_t interface;
const char *esp_err_to_name(int err) { return "test error"; }
int64_t esp_timer_get_time(void) { return now; }
uint32_t esp_random(void) { return 5; }
esp_err_t load_wifi(char ssid[33],char password[65]) { if(load_error)return load_error;strcpy(ssid,stored_ssid);strcpy(password,stored_password);return ESP_OK; }
esp_err_t save_wifi(const char *ssid,const char *password) {
    if (save_failure) return ESP_FAIL;
    ++saves;strcpy(stored_ssid,ssid);strcpy(stored_password,password);return ESP_OK;
}
esp_err_t load_wifi_enabled(bool *value) { *value=stored_enabled;return ESP_OK; }
esp_err_t save_wifi_enabled(bool value) { if(save_failure)return ESP_FAIL;stored_enabled=value;return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &interface; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { return &interface; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base,int event,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg) { return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { return ESP_OK; }
esp_err_t esp_netif_sntp_start(void) { return ESP_OK; }
void esp_netif_sntp_deinit(void) {}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { if(ap_error)return ap_error;memcpy(ap->ssid,current.sta.ssid,32);return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *out) {
    *count=3;out[0]=(wifi_ap_record_t){.ssid="new-home",.rssi=-50,.authmode=WIFI_AUTH_WPA2_PSK};
    out[1]=out[0];out[2]=(wifi_ap_record_t){.ssid="guest",.rssi=-70,.authmode=WIFI_AUTH_OPEN};return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int value) { mode=value;return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *config) { if(iface==WIFI_IF_STA)current=*config;return ESP_OK; }
esp_err_t esp_wifi_start(void) { if(start_error)return start_error;radio_running=true; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { if(stop_error)return stop_error;radio_running=false; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++connects;return connect_error; }
esp_err_t esp_wifi_disconnect(void) {
    wifi_event_sta_disconnected_t event={.reason=WIFI_REASON_ASSOC_LEAVE};
    wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const void *config,bool block) { assert(!block);return scan_failure?ESP_FAIL:ESP_OK; }
esp_err_t esp_wifi_scan_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *list) { list->num=1;return ESP_OK; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,esp_netif_ip_info_t *info) { info->ip.addr=0x0104a8c0;return ESP_OK; }
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif) { return ESP_OK; }
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif) { return ESP_OK; }
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif,int type,const esp_netif_dns_info_t *info) { return ESP_OK; }
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif,int operation,int option,void *value,size_t length) { assert(length == 1 && *(uint8_t *)value); return ESP_OK; }
esp_err_t portal_start(uint32_t address) { return ESP_OK; }
void portal_stop(void) {}

static void connected(void) {
    ip_event_got_ip_t event={.ip_info.ip.addr=0x0a01a8c0};
    wifi_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);
}
int main(void)
{
    network_init();connected();
    assert(network_open(false)==ESP_OK && !network_status().hotspot); // 已联网走局域网。
    assert(network_open(true)==ESP_OK && network_status().hotspot && mode==WIFI_MODE_APSTA);
    assert(network_status().client && !network_status().ip);
    assert(network_scan()==ESP_OK);
    assert(network_connect("new-home","password")==ESP_ERR_INVALID_STATE);
    network_poll();wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,NULL);
    assert(!network_status().scanning && network_status().count==2); // 去重，保留开放网络。
    scan_failure=true;assert(network_scan()==ESP_OK);network_poll();
    assert(!network_status().scanning);scan_failure=false;
    assert(network_connect("new-home","wrong-pass")==ESP_OK);network_poll();
    assert(!saves && network_status().connecting);
    wifi_event_sta_disconnected_t bad={.reason=200};
    wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&bad);network_poll();
    assert(!network_status().connecting && network_status().hotspot && !saves && !strcmp(stored_ssid,"old-home"));
    assert(network_connect("new-home","new-password")==ESP_OK);network_poll();connected();network_poll();
    assert(saves==1 && !strcmp(stored_ssid,"new-home") && network_status().hotspot && network_status().ip);
    // 下一次错误尝试、超时或关闭页面都不能覆盖刚保存的有效网络。
    assert(network_connect("timeout","password")==ESP_OK);network_poll();now+=26000000;network_poll();
    assert(!network_status().connecting && saves==1);
    assert(network_connect("unsaved","password")==ESP_OK);network_poll();connected();save_failure=true;network_poll();
    assert(!network_status().connecting && saves==1 && !strcmp(stored_ssid,"new-home"));
    save_failure=false;
    assert(network_connect("cancelled","password")==ESP_OK);network_close();network_poll();
    assert(!network_status().hotspot && !network_status().connecting && mode==WIFI_MODE_STA && saves==1);
    assert(!strcmp((char *)current.sta.ssid,"new-home"));
    // 没连上路由器时，统一入口自动提供热点，不要求用户先配网。
    assert(network_open(false)==ESP_OK && network_status().hotspot);
    network_close();
    assert(network_set_enabled(false)==ESP_OK && !network_status().enabled && !radio_running);
    int before=connects;
    network_poll();assert(connects==before);
    // 日常关闭时，网页仍能临时开热点；退出恢复关闭，网络凭据保持。
    assert(network_open(false)==ESP_OK && radio_running && network_status().hotspot);
    assert(!network_status().enabled);
    network_close();assert(!radio_running && !network_status().enabled && !strcmp(stored_ssid,"new-home"));
    save_failure=true;assert(network_set_enabled(true)==ESP_FAIL && !radio_running && !stored_enabled);save_failure=false;
    assert(network_set_enabled(true)==ESP_OK && radio_running && stored_enabled);
    // 断开后的旧 IP 事件不能让已关闭接口显示已连接。
    assert(network_set_enabled(false)==ESP_OK);
    ap_error=ESP_FAIL;connected();assert(!network_status().ip);ap_error=ESP_OK;
    // 驱动启动/连接/配置读取失败需反馈，并回退开关，保留凭据。
    start_error=ESP_ERR_NO_MEM;assert(network_set_enabled(true)==ESP_ERR_NO_MEM);
    assert(!network_enabled() && !stored_enabled && !radio_running);start_error=ESP_OK;
    connect_error=ESP_FAIL;assert(network_set_enabled(true)==ESP_FAIL);
    assert(!network_enabled() && !stored_enabled && !radio_running);connect_error=ESP_OK;
    load_error=ESP_FAIL;assert(network_set_enabled(true)==ESP_FAIL);
    assert(!network_enabled() && !stored_enabled);load_error=ESP_OK;
    // 停止失败保持原开关及连接信息，并阻止继续切换热点。
    assert(network_set_enabled(true)==ESP_OK);connected();stop_error=ESP_FAIL;
    assert(network_set_enabled(false)==ESP_FAIL && network_enabled() && radio_running && network_status().ip);
    assert(network_open(true)==ESP_FAIL && !network_hotspot_active());stop_error=ESP_OK;
    // 连续开关及临时热点切换沿用同一状态机，关闭后不发起后台重连。
    for(int i=0;i<12;++i) {
        assert(network_set_enabled(false)==ESP_OK && !network_enabled());
        before=connects;network_poll();assert(connects==before);
        assert(network_open(false)==ESP_OK && network_hotspot_active());
        network_close();assert(!radio_running && !network_enabled());
        assert(network_set_enabled(true)==ESP_OK && radio_running);connected();
    }
    assert(network_shutdown()==ESP_OK);
    puts("Network tests passed: automatic hotspot, scan, failed retry, verified save, timeout and cancellation.");
}
