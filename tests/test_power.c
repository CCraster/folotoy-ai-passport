#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/power.c"

static char sequence[64];
static size_t count;
static int button_mv = 3300, button_level = 1;
static char failing_step;
static jmp_buf terminal;
static esp_err_t step(char value) { sequence[count++] = value; sequence[count] = 0; return value == failing_step ? ESP_FAIL : ESP_OK; }
const char *esp_err_to_name(int err) { return "test error"; }
int bsp_button_read_mv(void) { return button_mv; }
esp_err_t esp_pm_configure(const esp_pm_config_t *config)
{ assert(config->max_freq_mhz == 160 && config->min_freq_mhz == 40 && !config->light_sleep_enable); return ESP_OK; }
bool bsp_lvgl_lock(int timeout) { return step('L') == ESP_OK; }
void bsp_lvgl_unlock(void) { step('U'); }
void bsp_display_backlight(uint8_t value) { step(value ? 'B' : 'b'); }
void *bsp_display_panel(void) { return sequence; }
esp_err_t esp_lcd_panel_disp_on_off(void *panel, bool on) { return step(on ? 'D' : 'd'); }
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t pins, int mode) { assert(pins == 1 && mode == ESP_GPIO_WAKEUP_GPIO_LOW); return step('G'); }
esp_err_t web_shutdown(void) { return step('W'); }
esp_err_t quota_ble_stop(void) { return step('Q'); }
esp_err_t music_prepare_deep_sleep(void) { return step('A'); }
esp_err_t bsp_battery_sleep(void) { return step('C'); }
esp_err_t bsp_i2c_prepare_deep_sleep(void) { return step('I'); }
esp_err_t bsp_button_prepare_deep_sleep(int *level) { *level = button_level; return step('K'); }
esp_err_t bsp_display_prepare_deep_sleep(void) { return step('S'); }
void esp_deep_sleep_start(void) { step('Z'); longjmp(terminal, 1); }
void esp_restart(void) { step('R'); longjmp(terminal, 2); }

static void reset_trace(void) { count = 0; sequence[0] = 0; failing_step = 0; }

int main(void)
{
    passport_preferences_t prefs = {.sleep_seconds=30,.deep_sleep_seconds=300,.power_mode=POWER_PORTABLE};
    assert(power_idle_action(&prefs,29,false,false)==IDLE_KEEP_RUNNING);
    assert(power_idle_action(&prefs,30,false,false)==IDLE_SCREEN_OFF);
    assert(power_idle_action(&prefs,299,true,false)==IDLE_KEEP_RUNNING);
    assert(power_idle_action(&prefs,300,true,false)==IDLE_DEEP_SLEEP);
    prefs.sleep_seconds=0;
    assert(power_idle_action(&prefs,300,false,false)==IDLE_DEEP_SLEEP);
    assert(power_idle_action(&prefs,300,false,true)==IDLE_KEEP_RUNNING);
    prefs.power_mode=POWER_DESKTOP;
    assert(power_idle_action(&prefs,3600,false,false)==IDLE_KEEP_RUNNING);
    prefs.sleep_seconds=60;
    assert(power_idle_action(&prefs,60,false,false)==IDLE_SCREEN_OFF);
    assert(power_idle_action(&prefs,3600,true,false)==IDLE_KEEP_RUNNING);
    power_init();
    assert(power_display(false, 70) == ESP_OK && !strcmp(sequence, "LbdU"));
    reset_trace();
    assert(power_display(true, 70) == ESP_OK && !strcmp(sequence, "LDBU"));
    reset_trace(); failing_step = 'D';
    assert(power_display(true, 70) == ESP_FAIL && !strchr(sequence, 'B'));
    reset_trace(); button_mv = 500;
    assert(power_deep_sleep() == ESP_ERR_INVALID_STATE && !count);
    button_mv = -1;
    assert(power_deep_sleep() == ESP_FAIL && !count);
    button_mv = 3300;
    const char *failures = "GLWQACIKS";
    for (size_t i = 0; i < strlen(failures); ++i) {
        reset_trace(); failing_step = failures[i];
        int result = setjmp(terminal);
        if (!result) {
            esp_err_t err = power_deep_sleep();
            assert((failing_step == 'G' || failing_step == 'L') && err != ESP_OK);
        } else assert(result == 2 && !strchr(sequence, 'Z'));
    }
    reset_trace(); button_level = 0;
    if (!setjmp(terminal)) { (void)power_deep_sleep(); assert(0); }
    assert(strchr(sequence, 'R') && !strchr(sequence, 'Z'));
    reset_trace(); button_level = 1;
    int result = setjmp(terminal);
    if (!result) { (void)power_deep_sleep(); assert(0); }
    assert(result == 1 && !strcmp(sequence, "GLWQACIKSZ"));
    puts("Power tests passed: held keys, display wake order, peripheral failures and terminal sleep handoff.");
}
