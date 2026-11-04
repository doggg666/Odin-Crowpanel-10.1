# Odin ESP-IDF firmware scaffold

This directory now has a minimal ESP-IDF project layout targeting the Elecrow
CrowPanel Advanced 10.1 hardware revision V1.0:

- Top-level `CMakeLists.txt` and `sdkconfig.defaults`
- `main/main.c` and its component CMake file
- Existing LVGL 8.3 UI scaffold in `odin_ui/`
- `board/` reserved for the exact Elecrow V1.0 BSP integration

## Build the current scaffold

Use ESP-IDF **v5.4.2** in an ESP-IDF PowerShell or VS Code terminal:

```powershell
cd CrowPanel-Odin-V1.0/firmware
idf.py set-target esp32p4
idf.py build
```

The current `main.c` is deliberately hardware-neutral. A successful build
only verifies the ESP-IDF project skeleton. It does **not** initialize the
display, touch controller, camera, microphone, speaker, amplifier, or ESP32-C6
Wi-Fi, and it is not yet the finished Odin firmware.

## Flashing

After a successful build, connect the panel over its programming USB port and
use the correct Windows COM port:

```powershell
idf.py -p COM5 flash monitor
```

Replace `COM5` with the port shown in Windows Device Manager.

## Next hardware integration step

Use Elecrow's official repository and only its `example/V1.0/idf-code/`
source as the board-support reference. Integrate its actual V1.0 BSP,
component dependencies, partition table, sdkconfig options and initialization
sequence before enabling the Odin LVGL UI or peripherals.

Do not borrow pin assignments or initialization from V1.1/V1.2. Do not
initialize display, touch, camera, PDM microphone, I2S speaker/amplifier, or
ESP-Hosted Wi-Fi a second time if the BSP already owns those resources.

The existing `odin_ui/odin_ui.c` uses LVGL 8.x APIs and must be added to the
firmware component build only after the Elecrow BSP has supplied and initialized
the matching LVGL 8.3 component.
