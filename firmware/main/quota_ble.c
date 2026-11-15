#include "passport.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

void ble_store_config_init(void);

static const ble_uuid128_t service_id = BLE_UUID128_INIT(
    0x81,0xce,0x34,0x7a,0x31,0x5c,0xb9,0xa5,0xf7,0x49,0x45,0x4d,0x21,0x83,0x00,0x9b);
static const ble_uuid128_t data_id = BLE_UUID128_INIT(
    0x81,0xce,0x34,0x7a,0x31,0x5c,0xb9,0xa5,0xf7,0x49,0x45,0x4d,0x22,0x83,0x00,0x9b);

static uint8_t own_addr_type;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool connected;
static bool stopping;
static bool enabled = true;
static bool initialized;
static bool paired;
static SemaphoreHandle_t stop_done;
static esp_err_t stop_result;
static int passkey = -1;
static int64_t pairing_deadline;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_event pairing_changed;
static RTC_DATA_ATTR quota_snapshot_t snapshot;
static RTC_DATA_ATTR reset_snapshot_t resets;
static sync_status_t sync_status;
static int64_t completion_until;

static uint32_t get_le(const uint8_t *bytes, size_t length)
{
    uint32_t result = 0;
    for (size_t i = 0; i < length; ++i) result |= (uint32_t)bytes[i] << (8 * i);
    return result;
}

static int receive_quota(uint16_t connection, uint16_t handle,
                         struct ble_gatt_access_ctxt *context, void *arg)
{
    uint8_t packet[20];
    uint16_t length;
    if (OS_MBUF_PKTLEN(context->om) != sizeof packet ||
        ble_hs_mbuf_to_flat(context->om, packet, sizeof packet, &length) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    quota_snapshot_t next = {.updated_at = get_le(packet, 4)};
    int16_t offset = (int16_t)get_le(packet + 4, 2);
    if (next.updated_at < 1700000000 || next.updated_at > 4102444800UL ||
        offset < -720 || offset > 840) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    for (size_t i = 0; i < 2; ++i) {
        const uint8_t *window = packet + 6 + 7 * i;
        next.windows[i] = (quota_window_t){
            .minutes = get_le(window, 2),
            .remaining = window[2],
            .reset_at = get_le(window + 3, 4),
        };
        if (next.windows[i].remaining > 100) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    set_clock(next.updated_at, offset);
    portENTER_CRITICAL(&lock);
    // A clock-only packet calibrates time while leaving the last quota visible.
    if (next.windows[0].minutes || next.windows[1].minutes) {
        snapshot = next;
        sync_status.quota_failed = false;
    }
    portEXIT_CRITICAL(&lock);
    if (next.windows[0].minutes || next.windows[1].minutes)
        ESP_LOGI("quota", "Codex quota received: %u window(s)",
                 (unsigned)((next.windows[0].minutes != 0) + (next.windows[1].minutes != 0)));
    return 0;
}

static int receive_resets(uint16_t connection, uint16_t handle,
                          struct ble_gatt_access_ctxt *context, void *arg)
{
    uint8_t packet[12];
    uint16_t length;
    reset_snapshot_t next;
    if (OS_MBUF_PKTLEN(context->om) != sizeof packet ||
        ble_hs_mbuf_to_flat(context->om, packet, sizeof packet, &length) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (!decode_reset_packet(packet, length, &next)) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    portENTER_CRITICAL(&lock);
    resets = next;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI("quota", "Reset-card summary received (known=%d)", next.known);
    return 0;
}

static int receive_status(struct ble_gatt_access_ctxt *context)
{
    uint8_t packet[SYNC_STATUS_BYTES];
    uint16_t length;
    sync_status_t next;
    if (ble_hs_mbuf_to_flat(context->om, packet, sizeof packet, &length) != 0 ||
        !decode_sync_status(packet, length, &next)) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    int64_t until = next.completed ? esp_timer_get_time() + 3000000 : 0;
    portENTER_CRITICAL(&lock);
    sync_status = next;
    if (until) completion_until = until;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI("quota", "Mac status: tasks=%u known=%d completed=%d quota_failed=%d",
             (unsigned)next.active, next.tasks_known, next.completed, next.quota_failed);
    return 0;
}

// 复用稳定的已配对特征，避免 macOS 的 GATT 缓存隐藏升级后新增的特征。
// 消息按固定长度分派，各解析器负责校验字段。
static int receive_message(uint16_t connection, uint16_t handle,
                           struct ble_gatt_access_ctxt *context, void *arg)
{
    if (OS_MBUF_PKTLEN(context->om) == SYNC_STATUS_BYTES) return receive_status(context);
    if (OS_MBUF_PKTLEN(context->om) == 12)
        return receive_resets(connection, handle, context, arg);
    return receive_quota(connection, handle, context, arg);
}

static const struct ble_gatt_chr_def characteristics[] = {
    {.uuid = &data_id.u, .access_cb = receive_message,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN},
    {0},
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_id.u, .characteristics = characteristics},
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    portENTER_CRITICAL(&lock);
    bool stopped = stopping;
    bool pairing = pairing_deadline > esp_timer_get_time();
    portEXIT_CRITICAL(&lock);
    if (stopped) return;

    // 首次连接由机身配对页开启，避免 Mac 在屏幕尚未显示验证码时弹出密码框。
    // 本函数只在 NimBLE host 任务中执行，配对存储与连接事件保持串行。
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int peer_count = 0;
    int stored = ble_store_util_bonded_peers(peers, &peer_count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (stored != 0) {
        ESP_LOGE("quota", "Read paired peers: %d", stored);
        return;
    }
    portENTER_CRITICAL(&lock);
    paired = peer_count > 0;
    portEXIT_CRITICAL(&lock);
    if (!pairing && !peer_count) {
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        if (connection_handle != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        ESP_LOGI("quota", "Waiting for on-device pairing window");
        return;
    }
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = (ble_uuid128_t *)&service_id,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_gap_adv_params parameters = {
        .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int result = ble_gap_adv_set_fields(&fields);
    if (result == 0) result = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &parameters, gap_event, NULL);
    if (result) ESP_LOGW("quota", "BLE advertising: %d", result);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) advertise();
        else connection_handle = event->connect.conn_handle;
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        portENTER_CRITICAL(&lock);
        connected = false;
        sync_status = (sync_status_t){0};
        completion_until = 0;
        passkey = -1;
        portEXIT_CRITICAL(&lock);
        advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        portENTER_CRITICAL(&lock);
        connected = event->enc_change.status == 0;
        if (connected) paired = true;
        passkey = -1;
        if (connected) pairing_deadline = 0;
        portEXIT_CRITICAL(&lock);
        ESP_LOGI("quota", "BLE encryption status: %d", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        portENTER_CRITICAL(&lock);
        bool allowed = pairing_deadline > esp_timer_get_time();
        portEXIT_CRITICAL(&lock);
        if (!allowed || event->passkey.params.action != BLE_SM_IOACT_DISP) {
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
        portENTER_CRITICAL(&lock);
        passkey = io.passkey;
        portEXIT_CRITICAL(&lock);
        if (ble_sm_inject_io(event->passkey.conn_handle, &io))
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        portENTER_CRITICAL(&lock);
        bool allowed = pairing_deadline > esp_timer_get_time();
        portEXIT_CRITICAL(&lock);
        if (!allowed) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc peer;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &peer)) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_store_util_delete_peer(&peer.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void sync_ble(void)
{
    if (ble_hs_id_infer_auto(0, &own_addr_type) == 0) advertise();
}

static void run_ble(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void refresh_pairing(struct ble_npl_event *event)
{
    advertise();
}

static esp_err_t start_ble(void)
{
    if (initialized) return ESP_OK;
    if (!stop_done) stop_done = xSemaphoreCreateBinary();
    if (!stop_done) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&lock);
    stopping = false;
    connected = false;
    sync_status = (sync_status_t){0};
    completion_until = 0;
    passkey = -1;
    pairing_deadline = 0;
    portEXIT_CRITICAL(&lock);
    connection_handle = BLE_HS_CONN_HANDLE_NONE;
    // ESP-IDF 在 port 初始化时注册 NPL 函数表，事件必须在它之后创建。
    ble_npl_event_init(&pairing_changed, refresh_pairing, NULL);
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("Passport");
    ble_hs_cfg.sync_cb = sync_ble;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) return ESP_ERR_NO_MEM;
    nimble_port_freertos_init(run_ble);
    initialized = true;
    return ESP_OK;
}

void quota_ble_init(void)
{
    ESP_ERROR_CHECK(load_ble_enabled(&enabled));
    if (enabled) ESP_ERROR_CHECK(start_ble());
    else ESP_LOGI("quota", "Bluetooth disabled by saved preference");
}

static void stop_ble_task(void *arg)
{
    stop_result = nimble_port_stop();
    if (stop_result == ESP_OK) {
        // host 停止后、NPL 内存池释放前销毁应用事件，重新开启时分配新句柄。
        ble_npl_event_deinit(&pairing_changed);
        stop_result = nimble_port_deinit();
    }
    xSemaphoreGive(stop_done);
    vTaskDelete(NULL);
}

esp_err_t quota_ble_stop(void)
{
    if (!initialized) return ESP_OK;
    portENTER_CRITICAL(&lock);
    stopping = true;
    pairing_deadline = 0;
    portEXIT_CRITICAL(&lock);
    // NimBLE 内部停机等待没有超时，隔离到终端任务，由调用方在超时后重启恢复。
    if (xTaskCreate(stop_ble_task, "quota_stop", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(stop_done, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (stop_result == ESP_OK) {
        initialized = false;
        portENTER_CRITICAL(&lock);
        connected = false;
        sync_status = (sync_status_t){0};
        completion_until = 0;
        passkey = -1;
        portEXIT_CRITICAL(&lock);
    }
    return stop_result;
}

bool quota_ble_enabled(void)
{
    portENTER_CRITICAL(&lock);
    bool result = enabled;
    portEXIT_CRITICAL(&lock);
    return result;
}

bool quota_paired(void)
{
    portENTER_CRITICAL(&lock);
    bool result = paired;
    portEXIT_CRITICAL(&lock);
    return result;
}

esp_err_t quota_ble_set_enabled(bool value)
{
    bool previous = quota_ble_enabled();
    if (previous == value) return ESP_OK;
    // 持久化成功后才操作无线电；异常时恢复旧偏好并重启，避免停在半初始化状态。
    esp_err_t err = save_ble_enabled(value);
    if (err != ESP_OK) return err;
    err = value ? start_ble() : quota_ble_stop();
    if (err != ESP_OK) {
        esp_err_t restored = save_ble_enabled(previous);
        ESP_LOGE("quota", "Bluetooth transition: %s, restore preference: %s; restarting",
                 esp_err_to_name(err), esp_err_to_name(restored));
        esp_restart();
        return err;
    }
    portENTER_CRITICAL(&lock);
    enabled = value;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI("quota", "Bluetooth %s", value ? "enabled" : "disabled");
    return ESP_OK;
}

void quota_pair(void)
{
    if (!quota_ble_enabled() || !initialized) return;
    portENTER_CRITICAL(&lock);
    pairing_deadline = esp_timer_get_time() + 120000000;
    portEXIT_CRITICAL(&lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &pairing_changed);
}

void quota_end_pairing(void)
{
    if (!initialized) return;
    portENTER_CRITICAL(&lock);
    pairing_deadline = 0;
    passkey = -1;
    portEXIT_CRITICAL(&lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &pairing_changed);
}

bool quota_connected(void)
{
    portENTER_CRITICAL(&lock);
    bool result = connected;
    portEXIT_CRITICAL(&lock);
    return result;
}

int quota_passkey(void)
{
    portENTER_CRITICAL(&lock);
    int result = pairing_deadline > esp_timer_get_time() ? passkey : -1;
    portEXIT_CRITICAL(&lock);
    return result;
}

quota_snapshot_t quota_snapshot(void)
{
    portENTER_CRITICAL(&lock);
    quota_snapshot_t result = snapshot;
    portEXIT_CRITICAL(&lock);
    return result;
}

reset_snapshot_t quota_resets(void)
{
    portENTER_CRITICAL(&lock);
    reset_snapshot_t result = resets;
    portEXIT_CRITICAL(&lock);
    return result;
}


sync_status_t quota_sync_status(int64_t *until)
{
    portENTER_CRITICAL(&lock);
    sync_status_t result = sync_status;
    if (until) *until = completion_until;
    portEXIT_CRITICAL(&lock);
    return result;
}
