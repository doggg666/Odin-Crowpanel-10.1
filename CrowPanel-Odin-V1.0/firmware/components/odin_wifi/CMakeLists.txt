# The ESP32-P4 requires an ESP32-C6 hosted Wi-Fi driver. Do not
# pull in esp_wifi on a bare P4 build where the hosted driver is absent.
if(CONFIG_ODIN_WIFI_ENABLE)
    idf_component_register(
        SRCS "odin_wifi.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_wifi esp_event esp_netif nvs_flash
    )
else()
    idf_component_register(INCLUDE_DIRS "include")
endif()
