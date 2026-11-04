# Firmware integration notes

This folder is an LVGL 8.3 UI scaffold only. It is not a standalone ESP-IDF project and cannot be flashed by itself.

1. Clone Elecrow's official repository.
2. Open an example under `example/V1.0/idf-code/`.
3. Build and flash the unmodified example first.
4. Copy `odin_ui.c` and `odin_ui.h` into that project's `main/`.
5. Add `odin_ui.c` to the existing component's CMake source list.
6. Call `odin_ui_create()` only after Elecrow's LVGL display initialization has completed.
7. Call the setter functions from the same LVGL task/context used by the BSP.

Do not initialize LVGL, DSI, touch, camera, PDM, I2S, amplifier or ESP-Hosted Wi-Fi a second time. Keep the V1.0 source's board initialization and Kconfig settings intact.

This scaffold uses LVGL 8.x APIs. It deliberately avoids assumed pin numbers and camera driver setup.
