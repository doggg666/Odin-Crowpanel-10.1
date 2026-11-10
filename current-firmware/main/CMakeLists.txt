idf_component_register(
    SRCS "main.c" "board.c" "settings.c" "net.c" "odin.c" "camera.c" "ui.c" "wav.c" "remote.c" "microphone.c" "web.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web.html"
    REQUIRES driver esp_hw_support esp_lcd esp_lcd_ek79007 esp_lcd_touch_gt911
             esp_lvgl_port lvgl nvs_flash esp_wifi esp_netif esp_event
             esp_http_client esp-tls mbedtls json esp_timer esp_driver_i2s esp_video esp_cam_sensor esp_http_server esp_driver_jpeg)

# Pinned esp_video 1.4.1 uses this accessor for sensor-format enumeration.
idf_component_get_property(video_dir espressif__esp_video COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${video_dir}/private_include")

