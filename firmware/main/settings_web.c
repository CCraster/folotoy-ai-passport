#include "passport.h"
#include "music.h"
#include "network.h"
#include "web_code.h"
#include "web_peer.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

extern const uint8_t settings_html_start[] asm("_binary_settings_html_gz_start");
extern const uint8_t settings_html_end[] asm("_binary_settings_html_gz_end");
static httpd_handle_t server;
static char token[25];
static web_code_t access_code;
static int64_t activity_at;
static portMUX_TYPE activity_lock = portMUX_INITIALIZER_UNLOCKED;

void web_init(void) { network_init(); }

static void touch(void)
{
    portENTER_CRITICAL(&activity_lock);
    activity_at = esp_timer_get_time();
    portEXIT_CRITICAL(&activity_lock);
}

void web_poll(void) { network_poll(); }

bool web_expired(void)
{
    portENTER_CRITICAL(&activity_lock);
    bool expired = esp_timer_get_time() - activity_at >= 900000000;
    portEXIT_CRITICAL(&activity_lock);
    return expired && !network_status().connecting;
}

static void private_response(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
}

// 内嵌网页可指定自身语言，路由读取设备语言作为默认值。
static const char *web_text(httpd_req_t *req, const char *zh, const char *en)
{
    char locale[6];
    if (httpd_req_get_hdr_value_str(req, "X-Passport-Language", locale, sizeof locale) == ESP_OK) {
        if (!strcmp(locale, "en")) return en;
        if (!strcmp(locale, "zh-CN")) return zh;
    }
    return ui_text(zh, en);
}

static bool has_token(httpd_req_t *req)
{
    char query[40], supplied[25];
    return httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "k", supplied, sizeof supplied) == ESP_OK &&
        strlen(supplied) == sizeof token - 1 && strcmp(token, supplied) == 0;
}

// AP 入网密码已经由机身提供；只对直接连接卡片热点的客户端自动打开网页。
// 局域网请求仍须扫描含随机令牌的二维码，或输入机身六位访问码。
static bool from_hotspot(httpd_req_t *req)
{
    network_status_t net = network_status();
    if (!net.hotspot) return false;
    struct sockaddr_storage local = {0}, peer = {0};
    socklen_t local_size = sizeof local, peer_size = sizeof peer;
    int socket = httpd_req_to_sockfd(req);
    if (getsockname(socket, (struct sockaddr *)&local, &local_size)) return false;
    if (getpeername(socket, (struct sockaddr *)&peer, &peer_size)) return false;
    return web_peer_is_hotspot(&local, local_size, &peer, peer_size, net.ap_ip);
}

// 直接从 flash 分块发送，不为整个图片分配 RAM；总时限防止坏连接独占 HTTP 任务。
// 日志只记资源名，不记录 URL 中的会话令牌。
static esp_err_t send_resource(httpd_req_t *req, const void *data, size_t length, const char *name)
{
    int64_t started = esp_timer_get_time();
    size_t sent = 0;
    esp_err_t err = ESP_OK;
    while (sent < length) {
        if (esp_timer_get_time() - started >= 20000000) { err = ESP_ERR_TIMEOUT; break; }
        size_t count = length - sent < 1024 ? length - sent : 1024;
        err = httpd_resp_send_chunk(req, (const char *)data + sent, count);
        if (err != ESP_OK) break;
        sent += count;
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI("web", "%s: %u/%u bytes, %lld ms, result=%s, heap=%u largest=%u", name,
             (unsigned)sent, (unsigned)length, (esp_timer_get_time() - started) / 1000,
             esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return err;
}

static esp_err_t enter_editor(httpd_req_t *req)
{
    web_status_t status = web_status();
    private_response(req);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", status.url);
    return httpd_resp_sendstr(req, "Open Passport settings");
}

static bool permitted(httpd_req_t *req)
{
    private_response(req);
    if (has_token(req)) {
        // 状态轮询保持只读，页面闲置时仍会到期；保存/预览等操作延长会话。
        if (req->method == HTTP_POST || req->method == HTTP_DELETE || !strncmp(req->uri, "/?", 2)) touch();
        return true;
    }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, web_text(req, "配置已关闭，请在卡片上重新打开设备配置", "Setup has closed. Reopen Device Setup on the Passport."));
    return false;
}

extern const char unlock_html_start[] asm("_binary_unlock_html_start");
static esp_err_t page(httpd_req_t *req)
{
    private_response(req);
    if (!has_token(req)) {
        if (from_hotspot(req)) return enter_editor(req);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req, unlock_html_start);
    }
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return send_resource(req, settings_html_start, settings_html_end - settings_html_start, "settings");
}

static esp_err_t get_language(httpd_req_t *req)
{
    private_response(req);
    return httpd_resp_sendstr(req, ui_text("zh-CN", "en"));
}

static esp_err_t put_language(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    char locale[6];
    size_t length = req->content_len, received = 0;
    if (length != 2 && length != 5)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "语言无效", "Invalid language."));
    while (received < length) {
        int n = httpd_req_recv(req, locale + received, length - received);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, web_text(req, "传输超时", "Transfer timed out."));
        received += n;
    }
    bool english = length == 2 && !memcmp(locale, "en", 2);
    if (!english && (length != 5 || memcmp(locale, "zh-CN", 5)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "语言无效", "Invalid language."));
    if (save_language(english ? PASSPORT_EN : PASSPORT_ZH) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "保存失败，请重试", "Unable to save. Try again."));
    return httpd_resp_sendstr(req, ui_text("zh-CN", "en"));
}

static esp_err_t unlock(httpd_req_t *req)
{
    private_response(req);
    if (esp_timer_get_time() < access_code.blocked_until) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, web_text(req, "尝试过多，请一分钟后重试", "Too many attempts. Try again in one minute."));
    }
    char body[12] = {0};
    if (req->content_len != 11) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "请输入六位访问码", "Enter the six-digit access code."));
    size_t received = 0;
    while (received < 11) {
        int count = httpd_req_recv(req, body + received, 11 - received);
        if (count <= 0) return ESP_FAIL;
        received += count;
    }
    const char *supplied = !strncmp(body, "code=", 5) ? body + 5 : "";
    if (web_code_check(&access_code, supplied, esp_timer_get_time()) != WEB_CODE_OK) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, web_text(req, "访问码不正确，请查看卡片屏幕", "Incorrect code. Check your Passport screen."));
    }
    touch();
    return enter_editor(req);
}

static esp_err_t captive(httpd_req_t *req, httpd_err_code_t error)
{
    if (!strcmp(req->uri, "/favicon.ico")) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    if (from_hotspot(req) && req->method == HTTP_GET) return enter_editor(req);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t get_profile(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    passport_profile_t value = profile_snapshot();
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(result, "name", value.name);
    cJSON_AddStringToObject(result, "subtitle", value.subtitle);
    cJSON_AddStringToObject(result, "bio", value.bio);
    cJSON_AddBoolToObject(result, "avatar", value.badge != 0);
    cJSON_AddNumberToObject(result, "avatarWidth", value.portrait ? AVATAR_WIDTH : LEGACY_AVATAR_SIZE);
    cJSON_AddNumberToObject(result, "avatarHeight", value.portrait ? AVATAR_HEIGHT : LEGACY_AVATAR_SIZE);
    photo_album_t album = photo_album_snapshot();
    unsigned mask = 0;
    for (unsigned i = 0; i < PHOTO_CAPACITY; ++i)
        if (album.slots[i]) mask |= 1u << i;
    cJSON_AddNumberToObject(result, "photoMask", mask);
    cJSON_AddNumberToObject(result, "photoSelected", album.selected);
    cJSON_AddNumberToObject(result, "photoCapacity", PHOTO_CAPACITY);
    cJSON_AddBoolToObject(result, "hotspot", network_hotspot_active());
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static int photo_index(httpd_req_t *req)
{
    char value[2];
    if (httpd_req_get_hdr_value_str(req, "X-Photo-Index", value, sizeof value) != ESP_OK ||
        value[0] < '0' || value[0] >= '0' + PHOTO_CAPACITY) return -1;
    return value[0] - '0';
}

static esp_err_t get_asset(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    enum { GET_AVATAR, GET_PHOTO, GET_BADGE };
    uintptr_t kind = (uintptr_t)req->user_ctx;
    bool photo = kind == GET_PHOTO;
    passport_profile_t value = profile_snapshot();
    uint8_t slot = value.badge;
    if (photo) {
        int index = photo_index(req);
        if (index < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "照片位置无效", "Invalid photo slot."));
        slot = photo_album_snapshot().slots[index];
    }
    const uint8_t *pixels = asset_data(photo, slot);
    if (!pixels) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image");
    if (kind == GET_AVATAR) pixels += BADGE_PIXELS;
    size_t length = value.portrait ? AVATAR_WIDTH * AVATAR_HEIGHT * 2 : LEGACY_AVATAR_SIZE * LEGACY_AVATAR_SIZE * 2;
    if (photo) length = photo_asset_size(slot);
    httpd_resp_set_type(req, photo && length != PHOTO_BYTES ? "image/jpeg" : "application/octet-stream");
    if (kind == GET_BADGE) length += BADGE_PIXELS;
    return send_resource(req, pixels, length, photo ? "photo" : kind == GET_BADGE ? "badge" : "avatar");
}

static esp_err_t get_music(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    boot_music_config_t config = music_config();
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(result, "enabled", config.enabled);
    cJSON_AddNumberToObject(result, "selection", config.selection);
    cJSON_AddStringToObject(result, "custom", config.custom);
    cJSON_AddNumberToObject(result, "volume", music_volume());
    cJSON *presets = cJSON_AddArrayToObject(result, "presets");
    for (size_t i = 0; i < MUSIC_PRESET_COUNT; ++i)
        cJSON_AddItemToArray(presets, cJSON_CreateString(web_text(req, music_presets[i].name, music_presets[i].english_name)));
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t read_music(httpd_req_t *req, boot_music_config_t *config)
{
    if (!req->content_len || req->content_len > 4096) return ESP_ERR_INVALID_ARG;
    char *body = malloc(req->content_len + 1);
    if (!body) return ESP_ERR_NO_MEM;
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) { free(body); return ESP_ERR_TIMEOUT; }
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) { free(body); return ESP_ERR_TIMEOUT; }
        received += count;
    }
    body[received] = 0;
    // cJSON 的字符串接口以 NUL 结尾；拒绝嵌入 NUL，避免乐谱被静默截断。
    bool invalid = memchr(body, 0, received) || strstr(body, "\\u0000");
    cJSON *json = invalid ? NULL : cJSON_ParseWithOpts(body, NULL, true);
    free(body);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON *selection = cJSON_GetObjectItemCaseSensitive(json, "selection");
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(json, "custom");
    bool valid = cJSON_IsBool(enabled) && cJSON_IsNumber(selection) && cJSON_IsString(custom) &&
                 selection->valuedouble >= 0 && selection->valuedouble <= MUSIC_CUSTOM &&
                 selection->valuedouble == selection->valueint && strlen(custom->valuestring) <= RTTTL_MAX_BYTES;
    if (valid) {
        config->enabled = cJSON_IsTrue(enabled);
        config->selection = selection->valueint;
        strcpy(config->custom, custom->valuestring);
    }
    cJSON_Delete(json);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t check_music(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    boot_music_config_t config = {0};
    music_score_t score;
    if (read_music(req, &config) != ESP_OK || !music_validate(&config, &score))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "备份的乐谱无效", "The backup has an invalid melody."));
    return httpd_resp_sendstr(req, web_text(req, "乐谱已校验", "Melody verified."));
}

static esp_err_t put_music(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    boot_music_config_t config = {0};
    esp_err_t err = read_music(req, &config);
    bool preview = req->user_ctx != NULL;
    if (err == ESP_OK) err = preview ? music_preview(&config) : music_save(&config);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "乐谱无效：支持 4–7 八度、最多 96 个音符、总长 20 秒", "Invalid melody. Use octaves 4–7, up to 96 notes and 20 seconds."));
    if (err == ESP_ERR_NOT_ALLOWED)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "当前已静音，请先在卡片上调高音量", "Sound is muted. Turn up the volume on your Passport."));
    if (err != ESP_OK) {
        ESP_LOGE("settings", "Music %s: %s", preview ? "preview" : "save", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, preview ? web_text(req, "暂时无法播放，请重试", "Unable to play. Try again.") : web_text(req, "保存失败，请重试", "Unable to save. Try again."));
    }
    return httpd_resp_sendstr(req, preview ? web_text(req, "正在卡片上试听", "Playing on your Passport.") : web_text(req, "已保存", "Saved"));
}

static esp_err_t stop_music(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    music_stop();
    return httpd_resp_sendstr(req, web_text(req, "已停止", "Stopped."));
}

static esp_err_t get_device(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    passport_preferences_t value = preferences_snapshot();
    char json[512], storage[280] = "null";
    storage_usage_t usage;
    if (storage_usage(&usage) == ESP_OK)
        snprintf(storage, sizeof storage, "{\"total\":%lu,\"firmware\":%lu,\"firmwareCapacity\":%lu,\"assetsCapacity\":%lu,\"badge\":%lu,\"photo\":%lu}",
                 (unsigned long)usage.total, (unsigned long)usage.firmware, (unsigned long)usage.firmware_capacity,
                 (unsigned long)usage.assets_capacity, (unsigned long)usage.badge, (unsigned long)usage.photo);
    snprintf(json, sizeof json, "{\"brightness\":%u,\"volume\":%u,\"sleepSeconds\":%u,\"powerMode\":%u,\"deepSleepSeconds\":%u,\"storage\":%s}",
             value.brightness, value.volume, value.sleep_seconds, value.power_mode, value.deep_sleep_seconds, storage);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static bool json_integer(const cJSON *object, const char *key, int min, int max, int *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < min || value->valuedouble > max ||
        value->valuedouble != value->valueint) return false;
    *out = value->valueint;
    return true;
}

static esp_err_t put_device(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    if (!req->content_len || req->content_len > 256)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "设置格式有误", "Invalid settings."));
    char body[257];
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, web_text(req, "传输超时", "Transfer timed out."));
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, web_text(req, "传输超时", "Transfer timed out."));
        received += count;
    }
    body[received] = 0;
    cJSON *json = memchr(body, 0, received) ? NULL : cJSON_ParseWithOpts(body, NULL, true);
    int brightness, volume, seconds, mode;
    int deep_seconds = preferences_snapshot().deep_sleep_seconds;
    bool valid = json_integer(json, "brightness", 10, 100, &brightness) &&
                 json_integer(json, "volume", 0, 100, &volume) &&
                 json_integer(json, "sleepSeconds", 0, 300, &seconds) &&
                 json_integer(json, "powerMode", POWER_DESKTOP, POWER_PORTABLE, &mode);
    if (cJSON_HasObjectItem(json, "deepSleepSeconds"))
        valid = valid && json_integer(json, "deepSleepSeconds", 300, 1800, &deep_seconds);
    cJSON_Delete(json);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "设置格式有误", "Invalid settings."));
    passport_preferences_t value = {.brightness = brightness, .volume = volume, .sleep_seconds = seconds, .power_mode = mode, .deep_sleep_seconds = deep_seconds};
    esp_err_t err = update_preferences(PREF_DEVICE, &value);
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "请选择有效的时间", "Select a valid duration."));
    if (err != ESP_OK) {
        ESP_LOGE("settings", "Device settings save: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "保存失败，请重试", "Unable to save. Try again."));
    }
    return httpd_resp_sendstr(req, web_text(req, "已保存", "Saved"));
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool read_text(httpd_req_t *req, const char *key, char *out, size_t limit)
{
    char encoded[722];
    if (httpd_req_get_hdr_value_str(req, key, encoded, sizeof encoded) != ESP_OK || encoded[0] != '.') return false;
    size_t count = 0;
    for (size_t i = 1; encoded[i]; ++i) {
        unsigned char ch = encoded[i];
        if (ch == '%') {
            if (!encoded[i + 1] || !encoded[i + 2]) return false;
            int first = hex_digit(encoded[i + 1]), second = hex_digit(encoded[i + 2]);
            if (first < 0 || second < 0) return false;
            ch = first * 16 + second;
            i += 2;
        }
        if (count + 1 >= limit || (ch < 32 && ch != '\n') || ch == 127) return false;
        out[count++] = ch;
    }
    out[count] = 0;
    return true;
}

static esp_err_t put_asset(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    bool photo = req->user_ctx != NULL;
    char name[sizeof profile.name] = {0}, bio[sizeof profile.bio] = {0}, crc[9];
    char subtitle[sizeof profile.subtitle] = {0};
    bool has_subtitle = httpd_req_get_hdr_value_len(req, "X-Badge-Subtitle") > 0;
    if (httpd_req_get_hdr_value_str(req, "X-Image-CRC32", crc, sizeof crc) != ESP_OK ||
        strlen(crc) != 8 || strspn(crc, "0123456789abcdefABCDEF") != 8 ||
        (!photo && (!read_text(req, "X-Badge-Name", name, sizeof name) ||
                    (has_subtitle && !read_text(req, "X-Badge-Subtitle", subtitle, sizeof subtitle)) ||
                    !read_text(req, "X-Badge-Bio", bio, sizeof bio)))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "资料格式有误", "Invalid profile data."));
    }
    uint32_t checksum = strtoul(crc, NULL, 16);
    int index = photo ? photo_index(req) : -1;
    if (photo && index < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "照片位置无效", "Invalid photo slot."));
    if (photo) ESP_LOGI("photo_diag", "upload path=%s hotspot_running=%d slot=%d bytes=%u",
                        from_hotspot(req) ? "hotspot" : "lan", network_hotspot_active(),
                        index + 1, (unsigned)req->content_len);
    esp_err_t err = photo ? receive_photo(req, index, checksum) :
        receive_badge(req, checksum, name, has_subtitle ? subtitle : NULL, bio);
    if (photo) ESP_LOGI("photo_diag", "upload result=%s", esp_err_to_name(err));
    if (photo && err == ESP_ERR_NO_MEM)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "卡片暂时没有足够内存处理照片，请稍后重试", "Your Passport is low on memory. Try again shortly."));
    if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "图片格式、长度或校验有误", "Invalid image format, size or checksum."));
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "保存失败，旧内容仍可使用", "Unable to save. Existing content is available."));
    return httpd_resp_sendstr(req, web_text(req, "已同步到卡片", "Synced to your Passport."));
}

static esp_err_t delete_photo(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    int index = photo_index(req);
    if (index < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "照片位置无效", "Invalid photo slot."));
    if (remove_photo(index) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "删除失败，请重试", "Unable to delete. Try again."));
    return httpd_resp_sendstr(req, web_text(req, "已删除照片", "Photo deleted."));
}

static esp_err_t delete_badge(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    if (remove_badge() != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "身份卡清除失败", "Unable to clear the ID card."));
    return httpd_resp_sendstr(req, web_text(req, "已清除身份卡", "ID card cleared."));
}

static esp_err_t select_album_photo(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    int index = photo_index(req);
    if (index < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "照片位置无效", "Invalid photo slot."));
    esp_err_t err = select_photo(index);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "该位置没有照片", "This slot is empty."));
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "选中照片保存失败", "Unable to select the photo."));
    return httpd_resp_sendstr(req, web_text(req, "已选中照片", "Photo selected."));
}

static esp_err_t put_wifi(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    char body[320];
    if (!network_hotspot_active() || !req->content_len || req->content_len >= sizeof body)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "请在临时热点模式下提交 Wi-Fi 设置", "Join the Passport hotspot to configure Wi-Fi."));
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return ESP_FAIL;
        received += count;
    }
    body[received] = 0;
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool valid = cJSON_IsString(ssid) && cJSON_IsString(password) &&
        strlen(ssid->valuestring) > 0 && strlen(ssid->valuestring) <= 32 &&
        (strlen(password->valuestring) == 0 ||
         (strlen(password->valuestring) >= 8 && strlen(password->valuestring) <= 63));
    esp_err_t err = valid ? network_connect(ssid->valuestring, password->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "请检查网络名称和密码", "Check the network name and password."));
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, web_text(req, "正在扫描或连接，请稍后重试", "Wi-Fi is busy. Try again shortly."));
    }
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, web_text(req, "连接请求失败", "Unable to start connecting."));
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, web_text(req, "正在连接…", "Connecting…"));
}

static esp_err_t put_time(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    char epoch[11], offset[6], *end;
    if (httpd_req_get_hdr_value_str(req, "X-Clock-Epoch", epoch, sizeof epoch) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Clock-Offset", offset, sizeof offset) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "时间格式有误", "Invalid time."));
    unsigned long seconds = strtoul(epoch, &end, 10);
    if (*end || seconds < 1700000000 || seconds > 4102444800UL)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "时间格式有误", "Invalid time."));
    long minutes = strtol(offset, &end, 10);
    if (*end || minutes < -720 || minutes > 840)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, web_text(req, "时区格式有误", "Invalid time zone."));
    set_clock(seconds, minutes);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t get_network(httpd_req_t *req)
{
    if (!permitted(req)) return ESP_OK;
    if (req->method == HTTP_POST) {
        if (network_scan() != ESP_OK) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, web_text(req, "正在扫描或连接，请稍后重试", "Wi-Fi is busy. Try again shortly."));
        }
    }
    network_status_t state = network_status();
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(result, "hotspot", state.hotspot);
    cJSON_AddBoolToObject(result, "wifiEnabled", state.enabled);
    cJSON_AddBoolToObject(result, "connected", state.ip != 0);
    cJSON_AddBoolToObject(result, "connecting", state.connecting);
    cJSON_AddBoolToObject(result, "scanning", state.scanning);
    cJSON_AddBoolToObject(result, "error", state.error);
    cJSON_AddStringToObject(result, "ssid", state.connected_ssid);
    const char *message = "";
    switch (state.message) {
    case NETWORK_SCAN_FAILED:
        message = web_text(req, "扫描失败，请重试", "Scan failed. Try again."); break;
    case NETWORK_CONNECTED_SAVED:
        message = web_text(req, "已连接并保存，可继续编辑", "Connected and saved. You can continue editing."); break;
    case NETWORK_SAVE_FAILED:
        message = web_text(req, "保存失败，原网络已保留", "Unable to save. The previous network was kept."); break;
    case NETWORK_CONNECT_FAILED:
        message = web_text(req, "连接失败，请检查密码和网络后重试", "Connection failed. Check the password and try again."); break;
    case NETWORK_MESSAGE_NONE: break;
    }
    cJSON_AddStringToObject(result, "message", message);
    cJSON *entries = cJSON_AddArrayToObject(result, "networks");
    for (unsigned i = 0; i < state.count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", state.entries[i].ssid);
        cJSON_AddNumberToObject(item, "signal", state.entries[i].signal);
        cJSON_AddBoolToObject(item, "secure", state.entries[i].secure);
        cJSON_AddItemToArray(entries, item);
    }
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

esp_err_t web_open(bool force_hotspot)
{
    if (server) web_close();
    esp_err_t opened = network_open(force_hotspot);
    if (opened != ESP_OK) return opened;
    for (int i = 0; i < 3; ++i)
        snprintf(token + i * 8, 9, "%08lx", (unsigned long)esp_random());
    access_code = (web_code_t){0};
    snprintf(access_code.value, sizeof access_code.value, "%06lu", (unsigned long)(esp_random() % 1000000));
    touch();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_req_hdr_len = 2304;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 5;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    static const httpd_uri_t routes[] = {
        {"/", HTTP_GET, page, NULL},
        {"/unlock", HTTP_POST, unlock, NULL},
        {"/language", HTTP_GET, get_language, NULL},
        {"/language", HTTP_POST, put_language, NULL},
        {"/network", HTTP_GET, get_network, NULL},
        {"/network", HTTP_POST, get_network, NULL},
        {"/profile", HTTP_GET, get_profile, NULL},
        {"/avatar", HTTP_GET, get_asset, NULL},
        {"/photo", HTTP_GET, get_asset, (void *)1},
        {"/badge", HTTP_GET, get_asset, (void *)2},
        {"/badge", HTTP_POST, put_asset, NULL},
        {"/badge", HTTP_DELETE, delete_badge, NULL},
        {"/photo", HTTP_POST, put_asset, (void *)1},
        {"/photo", HTTP_DELETE, delete_photo, NULL},
        {"/photo/select", HTTP_POST, select_album_photo, NULL},
        {"/wifi", HTTP_POST, put_wifi, NULL},
        {"/time", HTTP_POST, put_time, NULL},
        {"/music", HTTP_GET, get_music, NULL},
        {"/music", HTTP_POST, put_music, NULL},
        {"/music/check", HTTP_POST, check_music, NULL},
        {"/music/preview", HTTP_POST, put_music, (void *)1},
        {"/music/stop", HTTP_POST, stop_music, NULL},
        {"/device", HTTP_GET, get_device, NULL},
        {"/device", HTTP_POST, put_device, NULL},
    };
    config.max_uri_handlers = sizeof routes / sizeof routes[0];
    esp_err_t err = httpd_start(&server, &config);
    for (size_t i = 0; err == ESP_OK && i < config.max_uri_handlers; ++i)
        err = httpd_register_uri_handler(server, &routes[i]);
    if (err == ESP_OK) err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive);
    if (err != ESP_OK) web_close();
    return err;
}

void web_close(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    token[0] = 0;
    access_code = (web_code_t){0};
    network_close();
}

esp_err_t web_shutdown(void)
{
    web_close();
    return network_shutdown();
}

web_status_t web_status(void)
{
    network_status_t net = network_status();
    uint32_t ip = net.hotspot ? net.ap_ip : net.ip;
    web_status_t result = {.active = server != NULL, .hotspot = net.hotspot,
                          .wifi_connected = net.ip != 0, .client = net.client};
    memcpy(result.ssid, net.ssid, sizeof result.ssid);
    memcpy(result.password, net.password, sizeof result.password);
    if (result.active && ip) {
        esp_ip4_addr_t address = {.addr = ip};
        snprintf(result.short_url, sizeof result.short_url, "http://" IPSTR, IP2STR(&address));
        snprintf(result.url, sizeof result.url, "%s/?k=%s", result.short_url, token);
        memcpy(result.code, access_code.value, sizeof result.code);
    }
    return result;
}
