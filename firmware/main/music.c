#include "music.h"

#include <math.h>
#include <string.h>
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

const music_preset_t music_presets[MUSIC_PRESET_COUNT] = {
    {"清晨", "Morning:d=8,o=5,b=140:c,e,g,4c6"},
    {"星光", "Starlight:d=16,o=6,b=160:e,g,8c7"},
    {"跃动", "Bounce:d=16,o=5,b=156:a6,e6,8p,c6,a,8p,8e,8a"},
};

static boot_music_config_t current = {.enabled = 1};
static bool completion_enabled = true;
static uint8_t completion_volume = MUSIC_VOLUME_FOLLOW;
// 独立音量随播放命令传递，避免覆盖开机音乐与系统音量。
typedef struct {
    music_score_t score;
    uint8_t volume;
    uint32_t capture_session;
} audio_command_t;
static nvs_handle_t store;
static SemaphoreHandle_t config_lock;
static QueueHandle_t playback;
static SemaphoreHandle_t audio_stopped;
static esp_err_t shutdown_result;
#define AUDIO_SHUTDOWN UINT16_MAX
static portMUX_TYPE capture_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t capture_session;
static meow_voice_t capture_voice;
static portMUX_TYPE volume_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t output_volume;
static int16_t sine_table[256];
static const char *TAG = "music";

uint8_t music_volume(void)
{
    portENTER_CRITICAL(&volume_lock);
    uint8_t result = output_volume;
    portEXIT_CRITICAL(&volume_lock);
    return result;
}

void music_set_volume(uint8_t volume)
{
    portENTER_CRITICAL(&volume_lock);
    output_volume = volume;
    portEXIT_CRITICAL(&volume_lock);
}

bool music_validate(const boot_music_config_t *config, music_score_t *score)
{
    if (config->enabled > 1 || config->selection > MUSIC_CUSTOM) return false;
    size_t length = strnlen(config->custom, sizeof config->custom);
    if (length == sizeof config->custom) return false;
    if (length && !rtttl_parse(config->custom, length, score)) return false;
    if (config->selection == MUSIC_CUSTOM) return length != 0;
    const char *text = music_presets[config->selection].rtttl;
    return rtttl_parse(text, strlen(text), score);
}

boot_music_config_t music_config(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    boot_music_config_t result = current;
    xSemaphoreGive(config_lock);
    return result;
}

boot_music_status_t music_status(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    boot_music_status_t result = {current.enabled, current.selection, current.custom[0] != 0};
    xSemaphoreGive(config_lock);
    return result;
}

void music_stop(void)
{
    if (playback) {
        audio_command_t stop = {0};
        xQueueOverwrite(playback, &stop);
    }
}

esp_err_t music_prepare_deep_sleep(void)
{
    if (!playback) return bsp_audio_prepare_deep_sleep();
    audio_command_t command = {.score.count = AUDIO_SHUTDOWN};
    xQueueOverwrite(playback, &command);
    if (xSemaphoreTake(audio_stopped, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    return shutdown_result;
}

// 保存成功后才替换运行态；网页和机身按键共享此配置入口。
static esp_err_t save_locked(const boot_music_config_t *config)
{
    esp_err_t err = nvs_set_blob(store, "boot_music", config, sizeof *config);
    if (err == ESP_OK) err = nvs_commit(store);
    if (err == ESP_OK) {
        current = *config;
        music_stop();
    } else ESP_LOGE(TAG, "Save boot music: %s", esp_err_to_name(err));
    return err;
}

esp_err_t music_save(const boot_music_config_t *config)
{
    music_score_t score;
    if (!music_validate(config, &score)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    esp_err_t err = save_locked(config);
    xSemaphoreGive(config_lock);
    return err;
}

esp_err_t music_toggle(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    boot_music_config_t next = current;
    next.enabled = !next.enabled;
    esp_err_t err = save_locked(&next);
    xSemaphoreGive(config_lock);
    return err;
}

esp_err_t music_next(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    boot_music_config_t next = current;
    next.selection = (next.selection + 1) % (next.custom[0] ? MUSIC_PRESET_COUNT + 1 : MUSIC_PRESET_COUNT);
    esp_err_t err = save_locked(&next);
    xSemaphoreGive(config_lock);
    return err;
}

esp_err_t music_preview(const boot_music_config_t *config)
{
    audio_command_t command = {.volume = MUSIC_VOLUME_FOLLOW};
    if (!music_validate(config, &command.score)) return ESP_ERR_INVALID_ARG;
    if (!playback) return ESP_ERR_INVALID_STATE;
    if (!music_volume()) return ESP_ERR_NOT_ALLOWED;
    xQueueOverwrite(playback, &command);
    return ESP_OK;
}

esp_err_t music_select(uint8_t selection)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    boot_music_config_t next = current;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (selection < MUSIC_PRESET_COUNT || (selection == MUSIC_CUSTOM && next.custom[0])) {
        next.selection = selection;
        err = save_locked(&next);
    }
    xSemaphoreGive(config_lock);
    return err;
}

esp_err_t music_preview_current(void)
{
    boot_music_config_t config = music_config();
    return music_preview(&config);
}

esp_err_t music_preview_selection(uint8_t selection)
{
    boot_music_config_t config = music_config();
    config.selection = selection;
    return music_preview(&config);
}

void music_boot(void)
{
    boot_music_config_t config = music_config();
    if (config.enabled && music_volume()) {
        esp_err_t err = music_preview(&config);
        if (err != ESP_OK) ESP_LOGW(TAG, "Boot music: %s", esp_err_to_name(err));
    }
}

bool music_completion_enabled(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    bool result = completion_enabled;
    xSemaphoreGive(config_lock);
    return result;
}

esp_err_t music_set_completion_enabled(bool enabled)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (completion_enabled != enabled) {
        err = nvs_set_u8(store, "task_sound", enabled);
        if (err == ESP_OK) err = nvs_commit(store);
        if (err == ESP_OK) completion_enabled = enabled;
    }
    xSemaphoreGive(config_lock);
    return err;
}

uint8_t music_completion_volume(void)
{
    xSemaphoreTake(config_lock, portMAX_DELAY);
    uint8_t result = completion_volume;
    xSemaphoreGive(config_lock);
    return result;
}

esp_err_t music_set_completion_volume(uint8_t volume)
{
    if (volume > 100 && volume != MUSIC_VOLUME_FOLLOW) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (completion_volume != volume) {
        err = nvs_set_u8(store, "task_volume", volume);
        if (err == ESP_OK) err = nvs_commit(store);
        if (err == ESP_OK) completion_volume = volume;
    }
    xSemaphoreGive(config_lock);
    return err;
}

static uint8_t playback_volume(uint8_t volume)
{
    return volume == MUSIC_VOLUME_FOLLOW ? music_volume() : volume;
}

esp_err_t music_preview_completion(void)
{
    if (!playback) return ESP_ERR_INVALID_STATE;
    const audio_command_t command = {
        // D5、F♯5、G5，前两音短促，最后一音落在主音上。
        .score = {.count = 3, .notes = {{587, 80}, {740, 100}, {784, 250}}},
        .volume = music_completion_volume(),
    };
    if (!playback_volume(command.volume)) return ESP_ERR_NOT_ALLOWED;
    xQueueOverwrite(playback, &command);
    return ESP_OK;
}

void music_completion_chime(void)
{
    if (music_completion_enabled() && music_preview_completion() == ESP_OK)
        ESP_LOGI(TAG, "Task completion chime queued");
}

void music_volume_sample(void)
{
    if (!playback) return;
    static const audio_command_t command = {
        .score = {.count = 1, .notes = {{784, 80}}}, .volume = MUSIC_VOLUME_FOLLOW,
    };
    xQueueOverwrite(playback, &command);
}

static esp_err_t play_score(const audio_command_t *command)
{
    const music_score_t *score = &command->score;
    uint8_t volume = playback_volume(command->volume);
    if (!volume) return ESP_OK;
    esp_err_t err = bsp_audio_wake();
    if (err != ESP_OK) return err;
    bsp_audio_set_volume(volume);
    int16_t pcm[512];
    for (size_t n = 0; n < score->count; ++n) {
        music_note_t note = score->notes[n];
        uint32_t samples = note.duration_ms * 16u;
        uint32_t voiced = samples * 9 / 10, phase = 0;
        uint32_t step = ((uint64_t)note.hz << 32) / 16000;
        for (uint32_t offset = 0; offset < samples;) {
            // 每 32 ms 检查新命令，换曲、停止和静音均可及时打断。
            if (uxQueueMessagesWaiting(playback)) return ESP_OK;
            uint8_t updated = playback_volume(command->volume);
            if (!updated) return ESP_OK;
            if (updated != volume) { volume = updated; bsp_audio_set_volume(volume); }
            size_t count = samples - offset < 512 ? samples - offset : 512;
            for (size_t i = 0; i < count; ++i) {
                uint32_t at = offset + i;
                int envelope = 256;
                if (at < 80) envelope = at * 256 / 80;
                if (at < voiced && voiced - at < 240 && (int)((voiced - at) * 256 / 240) < envelope)
                    envelope = (voiced - at) * 256 / 240;
                pcm[i] = note.hz && at < voiced ? sine_table[phase >> 24] * envelope / 256 : 0;
                phase += step;
            }
            err = bsp_audio_write(pcm, count * sizeof *pcm);
            if (err != ESP_OK) return err;
            offset += count;
        }
    }
    return ESP_OK;
}

esp_err_t music_game_capture(bool active)
{
    if (!playback) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&capture_lock);
    uint32_t session = ++capture_session;
    if (!session) session = ++capture_session;
    capture_voice = (meow_voice_t){0};
    portEXIT_CRITICAL(&capture_lock);
    if (active) {
        const audio_command_t command = {.capture_session = session};
        xQueueOverwrite(playback, &command);
    }
    return ESP_OK;
}

meow_voice_t music_game_voice(void)
{
    portENTER_CRITICAL(&capture_lock);
    meow_voice_t voice = capture_voice;
    portEXIT_CRITICAL(&capture_lock);
    return voice;
}

static bool capture_publish(uint32_t session, meow_voice_t voice)
{
    portENTER_CRITICAL(&capture_lock);
    bool active = session == capture_session;
    if (active) capture_voice = voice;
    portEXIT_CRITICAL(&capture_lock);
    return active;
}

static esp_err_t capture_meow(uint32_t session)
{
    if (!capture_publish(session, (meow_voice_t){0})) return ESP_OK;
    bsp_audio_set_volume(0);
    esp_err_t err = bsp_audio_wake();
    meow_detector_t detector = {0};
    int16_t pcm[MEOW_PCM_SAMPLES];
    unsigned warmup = 4; // 跳过 codec 启动后的前 40 ms。
    while (err == ESP_OK && !uxQueueMessagesWaiting(playback) && capture_publish(session, detector.voice)) {
        err = bsp_audio_read(pcm, sizeof pcm, 45);
        if (err == ESP_OK) {
            if (warmup) --warmup;
            else meow_detect(&detector, pcm);
        }
    }
    if (err != ESP_OK) capture_publish(session, (meow_voice_t){.failed = true});
    return err;
}

static void audio_worker(void *arg)
{
    audio_command_t command;
    for (;;) {
        esp_err_t err = bsp_audio_sleep();
        if (err != ESP_OK) ESP_LOGE(TAG, "Audio idle: %s", esp_err_to_name(err));
        xQueueReceive(playback, &command, portMAX_DELAY);
        if (command.score.count == AUDIO_SHUTDOWN) {
            // 所有 PCM 写入均已结束；由同一个任务完成音频总线的最后一次操作。
            shutdown_result = bsp_audio_prepare_deep_sleep();
            xSemaphoreGive(audio_stopped);
            for (;;) vTaskSuspend(NULL);
        }
        if (command.capture_session) {
            err = capture_meow(command.capture_session);
            if (err != ESP_OK) ESP_LOGE(TAG, "Game microphone: %s", esp_err_to_name(err));
            continue;
        }
        if (!command.score.count) continue;
        err = play_score(&command);
        if (err != ESP_OK) ESP_LOGE(TAG, "Music playback: %s", esp_err_to_name(err));
    }
}

esp_err_t music_init(uint8_t volume)
{
    config_lock = xSemaphoreCreateMutex();
    if (!config_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open("passport_v1", NVS_READWRITE, &store);
    if (err != ESP_OK) return err;
    boot_music_config_t saved = {0};
    size_t size = sizeof saved;
    err = nvs_get_blob(store, "boot_music", &saved, &size);
    music_score_t score;
    if (err == ESP_OK) {
        if (size != sizeof saved || !music_validate(&saved, &score)) return ESP_ERR_INVALID_ARG;
        current = saved;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    uint8_t task_sound = 1;
    err = nvs_get_u8(store, "task_sound", &task_sound);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (task_sound > 1) return ESP_ERR_INVALID_ARG;
    completion_enabled = task_sound != 0;
    uint8_t task_volume = MUSIC_VOLUME_FOLLOW;
    err = nvs_get_u8(store, "task_volume", &task_volume);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (task_volume > 100 && task_volume != MUSIC_VOLUME_FOLLOW) return ESP_ERR_INVALID_ARG;
    completion_volume = task_volume;
    music_set_volume(volume);
    // 音频初始化失败只影响播放；配置入口仍可使用，设备其余功能继续运行。
    err = bsp_audio_init();
    if (err == ESP_OK) err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Audio init: %s", esp_err_to_name(err)); return ESP_OK; }
    for (size_t i = 0; i < 256; ++i) sine_table[i] = (int16_t)(sinf(i * 6.28318530718f / 256) * 6000);
    playback = xQueueCreate(1, sizeof(audio_command_t));
    if (!playback) return ESP_ERR_NO_MEM;
    audio_stopped = xSemaphoreCreateBinary();
    if (!audio_stopped) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_worker, "passport_audio", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(playback);
        playback = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
