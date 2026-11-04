# Odin — CrowPanel Advanced 10.1 V1.0

Windows-hosted AI backend plus an ESP-IDF firmware scaffold and LVGL 8.3 UI
integration code for the Elecrow CrowPanel Advanced 10.1 V1.0.

## Target
- CrowPanel Advanced 10.1 hardware V1.0: ESP32-P4 + ESP32-C6
- 1024×600 display, 16 MB flash, 32 MB PSRAM
- ESP-IDF 5.4.2 and LVGL 8.3
- Use only Elecrow's matching `example/V1.0/idf-code/` source; do not mix V1.1/V1.2 BSP settings.

Official sources:
- https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen
- https://www.elecrow.com/wiki/P4_7_9_101_AI.html

## Repository layout

- `ai-server/`: FastAPI backend for local Ollama/LM Studio chat, optional cloud chat, image endpoint and JSON text WebSocket.
- `firmware/CMakeLists.txt`: ESP-IDF project entry point.
- `firmware/main/`: minimal firmware entry point.
- `firmware/odin_ui/`: LVGL 8.3 Odin screen scaffold.
- `firmware/board/`: reserved for the exact Elecrow V1.0 BSP integration.
- `firmware/protocol.md`: panel/backend protocol.

## Build the current firmware scaffold

Install ESP-IDF **v5.4.2** and use an ESP-IDF terminal:

```powershell
cd CrowPanel-Odin-V1.0/firmware
idf.py set-target esp32p4
idf.py build
```

A successful build currently verifies only the hardware-neutral ESP-IDF
project skeleton. It does not yet bring up the panel or its peripherals. The
complete Elecrow V1.0 BSP, partition configuration, managed components and
board initialization must be integrated before this becomes functional,
flash-ready Odin firmware.

## Included
- FastAPI backend: local chat through Ollama or LM Studio, optional OpenAI-compatible cloud chat, image vision (LM Studio image support depends on the loaded model), health endpoint and JSON WebSocket text chat.
- LVGL 8.3 Odin screen scaffold.
- Minimal ESP-IDF 5.4.2 project structure.
- Protocol and V1.0 integration notes.

## Not implemented yet
The firmware does not yet initialize the board, ESP32-C6 Wi-Fi, display/touch,
camera, microphone or speaker. Camera capture/upload, microphone PCM streaming,
speech transcription, TTS playback, and full-duplex voice are also not
implemented. Preserve Elecrow's original V1.0 BSP, drivers, managed components,
partition table and sdkconfig when integrating hardware.

## Windows backend setup
Install Python 3.11+ and a suitable NVIDIA driver. For LM Studio, install it,
download and load a model, then start its **Local Server** (default
`http://127.0.0.1:1234`). In PowerShell from this folder:

```powershell
cd ai-server
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
Copy-Item .env.example .env
# Edit .env: set LM_STUDIO_MODEL to the exact model id shown by LM Studio.
# If using Ollama instead, set LOCAL_BACKEND=ollama and pull the configured models.
python -m uvicorn odin.main:app --host 0.0.0.0 --port 8000
```

LM Studio check: open `http://127.0.0.1:1234/v1/models` on the PC and copy the
returned model `id` into `LM_STUDIO_MODEL` in `.env`. Keep LM Studio bound to
localhost; the panel only needs access to Odin's FastAPI port.

API docs: http://127.0.0.1:8000/docs
Health: http://127.0.0.1:8000/api/v1/health

Set the panel to the Windows PC's LAN IPv4 address, not localhost. Allow TCP
8000 only on the trusted Windows Private network profile. This starter has no
authentication; do not expose it to the public internet. Keep cloud API keys
in the PC's `.env`, never in firmware.

## API
- `GET /api/v1/health`
- `POST /api/v1/chat`
- `POST /api/v1/vision` (multipart image upload)
- `WS /api/v1/ws` (JSON text chat only)

See `firmware/protocol.md`, `firmware/README.md`, and
`MODEL_AND_FIRMWARE_PLAN.md` for the staged integration plan.
