#include "bsp_backlight_pin.h"
#include "hal/gpio_ll.h"
#include "soc/io_mux_reg.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "Passport backlight shares UART0 TX; boot console must use USB Serial/JTAG"
#endif

// before_init 运行时 BSS 尚未清零，仅使用寄存器操作，保持深睡的 GPIO hold。
static void backlight_off(void)
{
    gpio_ll_set_level(&GPIO, BSP_LCD_BL, 0);
    gpio_ll_matrix_out_default(&GPIO, BSP_LCD_BL);
    gpio_ll_od_disable(&GPIO, BSP_LCD_BL);
    gpio_ll_pullup_dis(&GPIO, BSP_LCD_BL);
    gpio_ll_pulldown_en(&GPIO, BSP_LCD_BL);
    gpio_ll_func_sel(&GPIO, BSP_LCD_BL, PIN_FUNC_GPIO);
    gpio_ll_output_enable(&GPIO, BSP_LCD_BL);
}

// ESP-IDF bootloader 使用此符号将弱 hook 的实现纳入链接。
void bootloader_hooks_include(void) {}

void bootloader_before_init(void)
{
    backlight_off();
}

void bootloader_after_init(void)
{
    // USB 控制台初始化会释放默认 UART TX 并关闭输出，须重新拉低，
    // 使固件校验/加载期间始终灭背光，直到应用 LEDC 以 duty=0 接管。
    backlight_off();
    ESP_LOGI("passport_boot", "Backlight held off on GPIO%d", BSP_LCD_BL);
}
