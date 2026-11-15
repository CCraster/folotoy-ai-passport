#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 包含实现以注入 NVS、队列及 I2S 故障，验证真实播放和保存逻辑。
#include "../firmware/main/music.c"

static audio_command_t queued;
static unsigned pending, writes, stop_after, mute_after, fail_after;
static bool audible;
static unsigned mic_reads;
static bool mic_fail;
static esp_err_t commit_error;
static bool has_saved, has_task_sound, has_task_volume;
static uint8_t task_sound_value, task_volume_value, applied_volume;
static boot_music_config_t saved_config;

QueueHandle_t xQueueCreate(unsigned count, unsigned size) { assert(count == 1 && size == sizeof queued); return &queued; }
int xQueueOverwrite(QueueHandle_t queue, const void *item) { (void)queue; queued = *(const audio_command_t *)item; pending = 1; return 1; }
int xQueueReceive(QueueHandle_t queue, void *item, unsigned wait) { (void)queue; (void)wait; *(audio_command_t *)item = queued; pending = 0; return 1; }
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { (void)queue; return pending; }
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &queued; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &queued; }
void vTaskSuspend(void *task) { (void)task; abort(); }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) { (void)s; (void)wait; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) { (void)name; (void)mode; *handle = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{ (void)handle; assert(!strcmp(key, "boot_music")); if (!has_saved) return ESP_ERR_NVS_NOT_FOUND; memcpy(out, &saved_config, *size); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{ (void)handle; assert(!strcmp(key, "boot_music") && size == sizeof saved_config); if (!commit_error) { saved_config = *(const boot_music_config_t *)data; has_saved = true; } return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle;
    if (!strcmp(key, "task_volume")) {
        if (!has_task_volume) return ESP_ERR_NVS_NOT_FOUND;
        *out = task_volume_value;
        return ESP_OK;
    }
    assert(!strcmp(key, "task_sound"));
    if (!has_task_sound) return ESP_ERR_NVS_NOT_FOUND;
    *out = task_sound_value;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    if (!strcmp(key, "task_volume")) {
        if (!commit_error) { has_task_volume = true; task_volume_value = value; }
        return ESP_OK;
    }
    assert(!strcmp(key, "task_sound"));
    if (!commit_error) { has_task_sound = true; task_sound_value = value; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return commit_error; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
esp_err_t bsp_audio_init(void) { return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch) { assert(hz == 16000 && bits == 16 && ch == 1); return ESP_OK; }
esp_err_t bsp_audio_wake(void) { return ESP_OK; }
esp_err_t bsp_audio_read(void *pcm, size_t bytes, uint32_t timeout_ms)
{
    assert(bytes == MEOW_PCM_SAMPLES * sizeof(int16_t) && timeout_ms <= 50);
    ++mic_reads;
    if (mic_fail) return ESP_FAIL;
    int16_t *samples = pcm;
    for (unsigned i = 0; i < MEOW_PCM_SAMPLES; ++i) samples[i] = i % 2 ? 300 : -300;
    if (mic_reads == 40) {
        meow_voice_t voice = music_game_voice();
        assert(voice.ready && voice.onset == 1 && voice.stretch == 1);
        assert(music_game_capture(false) == ESP_OK);
    }
    return ESP_OK;
}
esp_err_t bsp_audio_sleep(void) { return ESP_OK; }
esp_err_t bsp_audio_prepare_deep_sleep(void) { return ESP_OK; }
void bsp_audio_set_volume(uint8_t percent) { assert(percent <= 100); applied_volume = percent; }
esp_err_t bsp_audio_write(const void *data, size_t bytes)
{
    assert(bytes <= 1024 && bytes % 2 == 0);
    const int16_t *pcm = data;
    for (size_t i = 0; i < bytes / 2; ++i) { assert(abs(pcm[i]) <= 6000); audible |= pcm[i] != 0; }
    ++writes;
    if (writes == stop_after) pending = 1;
    if (writes == mute_after) music_set_volume(0);
    return writes == fail_after ? ESP_FAIL : ESP_OK;
}

static void parse_contract(void)
{
    music_score_t score;
    const char *valid = "Example:d=8,o=5,b=120:c#,d.6,8p,4a4.";
    assert(rtttl_parse(valid, strlen(valid), &score));
    assert(score.count == 4);
    assert(score.notes[0].hz == 554 && score.notes[0].duration_ms == 250);
    assert(score.notes[1].hz == 1175 && score.notes[1].duration_ms == 375);
    assert(score.notes[2].hz == 0 && score.notes[2].duration_ms == 250);
    assert(score.notes[3].hz == 440 && score.notes[3].duration_ms == 750);
    const char *invalid[] = {"", ":d=4:c", "x:d=0:c", "x:o=3:c", "x:b=0:c", "x:o=8:c",
        "x:d=4,d=8:c", "x:d=8,:c", "x::", "x::c,", "x::3c", "x::p#", "x::b#", "x::c8",
        "x::c..", "x::c/", "x::1p,1p,1p,1p,1p,1p", "x:b=999999999999:c"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        assert(!rtttl_parse(invalid[i], strlen(invalid[i]), &score));
    const char nul[] = "x::c\0,d";
    assert(!rtttl_parse(nul, sizeof nul - 1, &score));
    char many[512] = "x:d=32,b=900:";
    for (size_t i = 0; i < RTTTL_MAX_NOTES; ++i) strcat(many, i ? ",c" : "c");
    assert(rtttl_parse(many, strlen(many), &score));
    strcat(many, ",c");
    assert(!rtttl_parse(many, strlen(many), &score));
    // 截断输入和随机字节覆盖解析器边界，配合 ASan/UBSan 检查越界。
    for (size_t n = 0; n < strlen(valid); ++n) (void)rtttl_parse(valid, n, &score);
    unsigned state = 123;
    char bytes[RTTTL_MAX_BYTES];
    for (size_t run = 0; run < 2000; ++run) {
        size_t size = run % sizeof bytes + 1;
        for (size_t i = 0; i < size; ++i) { state = state * 1664525u + 1013904223u; bytes[i] = state >> 24; }
        (void)rtttl_parse(bytes, size, &score);
    }
}

int main(void)
{
    parse_contract();
    assert(music_init(40) == ESP_OK);
    assert(music_game_capture(true) == ESP_OK && pending);
    uint32_t session = queued.capture_session;
    pending = 0;
    assert(capture_meow(session) == ESP_OK && mic_reads == 40);
    assert(!music_game_voice().ready);
    assert(capture_meow(session) == ESP_OK && mic_reads == 40);
    assert(music_game_capture(true) == ESP_OK);
    pending = 0;
    mic_fail = true;
    assert(capture_meow(queued.capture_session) == ESP_FAIL && music_game_voice().failed);
    assert(music_game_capture(false) == ESP_OK);
    assert(music_completion_enabled()); // 旧设备没有设置记录时默认有提示音。
    music_completion_chime();
    assert(pending && queued.volume == MUSIC_VOLUME_FOLLOW && queued.score.count == 3);
    assert(queued.score.notes[2].duration_ms > queued.score.notes[1].duration_ms);
    pending = 0;
    assert(music_set_completion_enabled(false) == ESP_OK && !music_completion_enabled());
    music_completion_chime();
    assert(!pending);
    assert(music_init(40) == ESP_OK && !music_completion_enabled());
    commit_error = ESP_FAIL;
    assert(music_set_completion_enabled(true) == ESP_FAIL && !music_completion_enabled());
    commit_error = ESP_OK;
    assert(music_set_completion_enabled(true) == ESP_OK && music_completion_enabled());
    music_set_volume(0);
    music_completion_chime();
    assert(!pending);
    assert(music_set_completion_volume(101) == ESP_ERR_INVALID_ARG);
    assert(music_set_completion_volume(30) == ESP_OK);
    assert(music_init(0) == ESP_OK && music_completion_volume() == 30);
    commit_error = ESP_FAIL;
    assert(music_set_completion_volume(50) == ESP_FAIL && music_completion_volume() == 30);
    commit_error = ESP_OK;
    music_completion_chime();
    assert(pending && queued.volume == 30 && !music_volume());
    pending = writes = 0;
    assert(play_score(&queued) == ESP_OK && applied_volume == 30 && writes > 2);
    assert(music_set_completion_volume(0) == ESP_OK && !pending);
    music_set_volume(40);
    music_completion_chime();
    assert(!pending);
    assert(music_set_completion_volume(MUSIC_VOLUME_FOLLOW) == ESP_OK);
    assert(music_init(40) == ESP_OK && music_completion_volume() == MUSIC_VOLUME_FOLLOW);
    boot_music_config_t config = music_config();
    music_score_t score;
    for (size_t i = 0; i < MUSIC_PRESET_COUNT; ++i) { config.selection = i; assert(music_validate(&config, &score)); }
    strcpy(config.custom, "Mine:d=8,o=5,b=120:c,g,e");
    config.selection = MUSIC_CUSTOM;
    assert(music_save(&config) == ESP_OK);
    pending = 0;
    boot_music_config_t preview = config;
    preview.selection = 1;
    assert(music_preview(&preview) == ESP_OK && pending);
    assert(music_status().selection == MUSIC_CUSTOM); // 试听不保存。
    commit_error = ESP_FAIL;
    assert(music_save(&preview) == ESP_FAIL && music_status().selection == MUSIC_CUSTOM);
    commit_error = ESP_OK;
    assert(music_select(1)==ESP_OK && music_status().selection==1 && music_status().has_custom);
    assert(!strcmp(music_config().custom, config.custom));
    assert(music_select(255)==ESP_ERR_INVALID_ARG && music_status().selection==1);
    commit_error=ESP_FAIL;
    assert(music_select(2)==ESP_FAIL && music_status().selection==1);
    commit_error=ESP_OK;
    assert(music_select(MUSIC_CUSTOM)==ESP_OK);
    pending=0;assert(music_preview_current()==ESP_OK && pending && music_status().selection==MUSIC_CUSTOM);
    // 切换试听只覆盖待播队列，既不保存选择，也不排成长队。
    assert(music_preview_selection(0)==ESP_OK && pending==1);
    assert(music_preview_selection(1)==ESP_OK && pending==1);
    assert(rtttl_parse(music_presets[1].rtttl, strlen(music_presets[1].rtttl), &score));
    assert(queued.volume == MUSIC_VOLUME_FOLLOW && queued.score.count == score.count && queued.score.notes[0].hz == score.notes[0].hz);
    assert(music_status().selection==MUSIC_CUSTOM && saved_config.selection==MUSIC_CUSTOM);
    assert(music_preview_selection(255)==ESP_ERR_INVALID_ARG);
    assert(music_toggle() == ESP_OK && !music_status().enabled);
    pending = 0;
    music_boot();
    assert(!pending);
    assert(music_init(40) == ESP_OK && !music_status().enabled && music_status().has_custom);
    music_set_volume(0);
    assert(music_preview(&config) == ESP_ERR_NOT_ALLOWED);
    assert(music_preview_selection(1)==ESP_ERR_NOT_ALLOWED);
    music_set_volume(40);
    assert(music_validate(&config, &score));
    audio_command_t command = {.volume = MUSIC_VOLUME_FOLLOW, .score = score};
    pending = writes = 0;
    assert(play_score(&command) == ESP_OK && audible && writes > 2);
    writes = 0; stop_after = 2;
    assert(play_score(&command) == ESP_OK && writes == 2);
    pending = writes = stop_after = 0; mute_after = 2;
    assert(play_score(&command) == ESP_OK && writes == 2 && !music_volume());
    writes = mute_after = 0; fail_after = 2; music_set_volume(40);
    assert(play_score(&command) == ESP_FAIL && writes == 2);
    puts("Music tests passed: RTTTL, preview, independent/follow volume, persistence, mute and cancellation.");
}
