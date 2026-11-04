# CrowPanel board support integration

This directory is reserved for the Elecrow CrowPanel Advanced 10.1 hardware
revision V1.0 board-support integration.

Do not copy pin maps or initialization code from V1.1 or V1.2. During the next
integration milestone, bring in the matching Elecrow V1.0 BSP, component
manifests, partition table, sdkconfig options, and required managed components.

The current firmware entry point is intentionally hardware-neutral. It proves
the ESP-IDF project structure, not the panel, touch, camera, microphone,
speaker, or ESP32-C6 Wi-Fi functionality.
