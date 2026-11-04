# Odin — CrowPanel Advanced 10.1 V1.0 starter

Windows-hosted AI backend plus an LVGL 8.3 UI integration scaffold for the Elecrow CrowPanel Advanced 10.1 V1.0. This is **not** a complete flashable ESP-IDF firmware project.

## Target
- CrowPanel Advanced 10.1 hardware V1.0: ESP32-P4 + ESP32-C6
- 1024×600 display, 16 MB flash, 32 MB PSRAM
- ESP-IDF 5.4.2 and LVGL 8.3
- Use only Elecrow's matching `example/V1.0/idf-code/` source; do not mix V1.1/V1.2 BSP settings.

Official sources:
- https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen
- https://www.elecrow.com/wiki/P4_7_9_101_AI.html

## Included
- FastAPI backend: local Ollama chat, optional OpenAI-compatible cloud chat, local Ollama image vision, health endpoint and JSON WebSocket text chat.
- LVGL 8.3 Odin screen scaffold.
- Protocol and V1.0 integration notes.

## Not implemented yet
The starter does not include board initialization, ESP32-C6 Wi-Fi client integration, camera capture/upload firmware, microphone PCM streaming, speech transcription, TTS playback, or full-duplex voice. The firmware folder is a scaffold only. Preserve Elecrow's original V1.0 BSP, display/touch/audio/camera drivers, managed components, partition table and sdkconfig.

## Windows backend setup
Install Python 3.11+, Ollama and a suitable NVIDIA driver. In PowerShell from this folder:

```powershell
cd ai-server
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
Copy-Item .env.example .env
ollama pull qwen3:8b
ollama pull qwen2.5vl:7b
python -m uvicorn odin.main:app --host 0.0.0.0 --port 8000
```

API docs: http://127.0.0.1:8000/docs
Health: http://127.0.0.1:8000/api/v1/health

Set the panel to the Windows PC's LAN IPv4 address, not localhost. Allow TCP 8000 only on the trusted Windows Private network profile. This starter has no authentication; do not expose it to the public internet. Keep cloud API keys in the PC's `.env`, never in firmware.

## API
- `GET /api/v1/health`
- `POST /api/v1/chat`
- `POST /api/v1/vision` (multipart image upload)
- `WS /api/v1/ws` (JSON text chat only)

See `firmware/protocol.md`, `firmware/README.md`, and `MODEL_AND_FIRMWARE_PLAN.md` for the staged integration plan.
