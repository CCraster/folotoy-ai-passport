#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define IP_EVENT ((esp_event_base_t)2)
enum { IP_EVENT_STA_GOT_IP=1, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_SCAN_DONE, ESP_EVENT_ANY_ID };
enum { WIFI_MODE_STA, WIFI_MODE_APSTA, WIFI_IF_STA, WIFI_IF_AP, WIFI_STORAGE_RAM, WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_REASON_ASSOC_LEAVE, ESP_NETIF_DNS_MAIN, ESP_IPADDR_TYPE_V4, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER };
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip;} esp_netif_ip_info_t;
typedef struct {esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
typedef struct {uint8_t reason;} wifi_event_sta_disconnected_t;
typedef struct {uint8_t ssid[33];int8_t rssi;int authmode;} wifi_ap_record_t;
typedef struct {unsigned num;} wifi_sta_list_t;
typedef struct {struct {uint8_t ssid[32],password[64];}sta;struct {uint8_t ssid[32],password[64],ssid_len,channel;int authmode,max_connection;}ap;} wifi_config_t;
typedef struct {int unused;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {bool start;} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) ((esp_sntp_config_t){0})
typedef struct {struct {int type;union {esp_ip4_addr_t ip4;}u_addr;}ip;} esp_netif_dns_info_t;
typedef int esp_netif_t;
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_event_handler_register(esp_event_base_t base,int event,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg);
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *out);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_scan_start(const void *config,bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *list);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,esp_netif_ip_info_t *info);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif,int type,const esp_netif_dns_info_t *info);
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif,int operation,int option,void *value,size_t length);
uint32_t esp_random(void);

#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 20
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 21
