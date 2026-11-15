idf_component_register(
    SRCS "main.c" "pixel_city.c" "storage.c" "photo_jpeg.c" "storage_usage.c" "preferences.c" "power.c" "settings_web.c" "web_peer.c" "network.c" "portal.c" "portal_dns.c" "web_code.c" "quota_ble.c" "quota_protocol.c" "music.c" "rtttl.c"
         "meow_delivery.c" "meow_view.c" "fonts/passport_game.c" "fonts/passport_font.c"
         "fonts/passport_heading.c" "fonts/passport_caption.c" "fonts/passport_quota.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_partition esp_http_server esp_wifi esp_event esp_netif bt json esp_timer esp_pm esp_lcd app_update bootloader_support spi_flash
    EMBED_TXTFILES "unlock.html"
    EMBED_FILES "art/pixel_city.rgb565" "art/pixel_scenes.rgb565"
)

# 栈溢出不由主机 ASan 覆盖；保留目标编译器的每函数栈报告供构建门禁检查。
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)

idf_build_get_property(python PYTHON)
set(settings_gzip "${CMAKE_CURRENT_BINARY_DIR}/settings.html.gz")
add_custom_command(OUTPUT "${settings_gzip}"
    COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/compress_web.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/settings.html" "${settings_gzip}"
    DEPENDS "settings.html" "../../tools/compress_web.py"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${settings_gzip}" BINARY RENAME_TO settings_html_gz)
