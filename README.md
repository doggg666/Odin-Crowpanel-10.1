# Current firmware

The [current Odin dashboard firmware](current-firmware/) is a complete ESP-IDF 5.5 project for the CrowPanel Advanced 10.1-inch V1.0. Build and verification instructions are in its README. The older scaffold remains below for reference.

# Odin — CrowPanel Advanced 10.1 AI Assistant

Starter integration project for the Elecrow CrowPanel Advanced 10.1 ESP32-P4 HMI, with a Windows-hosted local/cloud AI backend and LVGL 8.3 UI scaffold.

## Project archive

Download `Odin-project.zip` from the repository root and extract it. It contains the backend, firmware UI scaffold, protocol notes, and integration plan.

**Important:** This is a starter scaffold, not a complete flashable ESP-IDF firmware project. Preserve and integrate with the exact Elecrow V1.0 BSP; do not mix V1.1/V1.2 files. Audio streaming and the board-side camera/network client are future integration work.

See the archive's README for setup, model configuration, and V1.0 integration steps.