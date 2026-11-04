# Odin model selection and V1.0 firmware integration plan

## Host PC
RTX 4090 (24 GB VRAM), Windows 11, 16 GB system RAM.

The GPU is powerful enough for quantized 7B models and many larger models. The 16 GB system RAM is the constraint when multiple services/models are loaded. Start with one model loaded at a time, keep context length moderate (4K–8K initially), and avoid running large chat and vision models concurrently.

## Recommended initial model stack

- Chat: `qwen2.5:7b` in Ollama. Its default quantized package is a good low-latency baseline.
- Vision: `qwen2.5vl:7b` in Ollama. If you want less memory use, use `qwen2.5vl:3b`.
- Speech recognition: `faster-whisper` with `small` initially; upgrade to `medium` only after measuring latency and system memory. Use CUDA when its PyTorch/CTranslate2 installation is confirmed.
- VAD: `silero-vad` for local speech boundary detection.
- TTS: Piper for fast offline speech. Start with one English voice; voice and language choice can be changed later.
- Optional higher-quality TTS: Kokoro-82M after the basic end-to-end audio path works.

Install models:
```powershell
ollama pull qwen2.5:7b
ollama pull qwen2.5vl:7b
```
For a lighter vision model:
```powershell
ollama pull qwen2.5vl:3b
```

Update `.env`:
```dotenv
OLLAMA_CHAT_MODEL=qwen2.5:7b
OLLAMA_VISION_MODEL=qwen2.5vl:7b
```

Ollama's Qwen2.5-VL library lists 3B and 7B image-capable variants; the 7B Q4 model download is listed around 6 GB. Check current tags in the Ollama library because model tags and packaging may evolve. Keep only one of the chat/vision models resident at first if Windows system RAM is under pressure.

## Audio pipeline
The included starter does not yet implement live audio streaming. Add it in this order:
1. Use the microphone capture code from the Elecrow V1.0 AI voice example; preserve its exact sample format and driver configuration.
2. Send binary WebSocket PCM frames to the Windows backend. Agree on sample rate, channels, bit depth, sequence numbers and framing on both ends.
3. Backend runs Silero VAD and faster-whisper.
4. Send recognized text to Ollama.
5. Piper generates PCM chunks.
6. Stream audio chunks back to the panel; play them using the existing V1.0 I2S speaker driver.
7. Add barge-in: detect new speech during playback, stop the speaker and cancel the current response.

This is initially turn-based voice with streaming transport. Do not call it full-duplex until simultaneous capture/playback, cancellation and echo handling have been tested.

## V1.0 firmware integration
1. Clone the official Elecrow CrowPanel Advanced 10.1 repository.
2. Work only in `example/V1.0/idf-code/`.
3. Use the exact V1.0 AI voice/camera project where available. Elecrow's Lesson01 AI voice project uses ESP-IDF 5.4.2 and describes PDM microphone capture, I2S speaker output, CSI camera, Wi-Fi through the ESP32-C6 coprocessor, 16 MB flash, 32 MB PSRAM and `partitions/v2/16m.csv`.
4. Build and flash the unmodified project first.
5. Preserve the project's `main`, `managed_components`, `partitions`, `dependencies.lock`, `sdkconfig`, `sdkconfig.defaults*`, C6 hosted firmware and LVGL 8.3 dependencies.
6. Add `firmware/odin_ui/odin_ui.c` and `.h` to the existing main component. Call `odin_ui_create()` only after Elecrow's LVGL display initialization; call setters from the existing LVGL task/context.
7. Add Wi-Fi backend connection and WebSocket JSON client, then camera JPEG upload to `/api/v1/vision`.
8. Only then implement binary audio streaming and playback.

Do not use V1.1/V1.2 projects or copy camera, wireless, touch or audio pin definitions across revisions. The actual V1.0 project's compiled BSP and configuration are authoritative.

## Remaining integration work
The archive's Python backend is runnable for local text chat, optional cloud text chat, and local image vision. The LVGL file is a UI scaffold. This is not a complete flashable firmware image. The following must still be integrated and tested in Elecrow's V1.0 project:
- board initialization and LVGL task hookup;
- ESP32-C6 hosted Wi-Fi connection and reconnect logic;
- WebSocket client and JSON parsing;
- CSI camera capture/JPEG encoding and upload;
- PDM microphone binary streaming;
- VAD, transcription, TTS, playback and barge-in;
- physical display/touch/camera/audio test on the V1.0 unit.

## Security
Keep cloud API keys on the Windows host in `.env`. Do not expose port 8000 to the public internet. The starter has no authentication; use it only on a trusted LAN until a device token is added.
