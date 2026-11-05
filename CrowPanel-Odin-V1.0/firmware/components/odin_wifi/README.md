# Odin Wi-Fi station manager

Provides network scan, SSID/password connection, disconnect, bounded retry,
and optional credential persistence in NVS.

**ESP-IDF 6.0.1:** `CONFIG_ODIN_WIFI_ENABLE` defaults to `n` so the bare ESP32-P4 scaffold does not require a hosted Wi-Fi implementation. Enable it through menuconfig only after installing and validating a matching IDF 6.0.1-compatible ESP-Hosted / esp_wifi_remote stack. This module has not been hardware-tested.\n\n**Hardware integration required:** ESP32-P4 has no built-in Wi-Fi radio. The
CrowPanel V1.0 uses an ESP32-C6 wireless coprocessor. This component calls the
ESP-IDF Wi-Fi API, which must be supplied through the **matching V1.0
ESP-Hosted / esp_wifi_remote stack**. Do not compile this component into
the application until the V1.0 hosted transport, its C6 firmware, the
component dependencies, and NVS have been integrated and tested.

Once the V1.0 BSP is integrated, add `REQUIRES odin_wifi` to
`main/CMakeLists.txt`, initialize the C6 transport first, then call
`odin_wifi_init()` and optionally `odin_wifi_connect_saved()`.

A network scan is blocking and must be invoked from a worker task, not the
LVGL UI task. Never log or display passwords. Credentials saved in NVS are
not automatically encrypted; configure NVS encryption and flash security
before using this on untrusted devices. There is no on-screen Wi-Fi settings
dialog yet.
