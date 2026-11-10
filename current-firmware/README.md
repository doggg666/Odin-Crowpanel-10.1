# Odin dashboard firmware

ESP-IDF 5.5 firmware for the Elecrow CrowPanel Advanced 10.1-inch ESP32-P4 v1.0:
1024 × 600 EK79007 display, GT911 touch, ESP32-C6 Wi-Fi, SC2336 removable camera,
PDM microphone, and I2S speaker. LVGL is pinned to 9.1.0.

## Build and flash

```sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Managed components download from `main/idf_component.yml` and `dependencies.lock`.
The `sdkconfig` values provide initial defaults; saved NVS settings take precedence.
Existing device settings migrate to the new configuration without erasing Wi-Fi,
volume, display preferences, selected model, or the TTS address.

## Web settings

Open the board IP shown on Home or Settings, for example `http://192.168.1.153/`.
Sign in as **admin**, using the generated password shown on the board's Settings
page. Change it in the portal’s Password tab; after a custom password is saved,
the board hides the credentials, including after restarts. The username stays
**admin**. Setup Wi-Fi adopts the new password on the next reboot.
The password persists across restarts. API responses never return stored API
keys or Wi-Fi passwords. Empty API-key fields preserve the saved value; check the
remove-key box to clear one.

The board also keeps an **Odin-XXXX** setup Wi-Fi network available. Its password is
the same web password. Join it and open `http://192.168.4.1/` if the board cannot
connect to your home network. Wi-Fi credentials entered in the web page are saved
only after the new connection obtains an IP address.

The web page works without external scripts, stylesheets, or a CDN. It provides:

- Nearby Wi-Fi scanning and network connection.
- Country/city search using Open-Meteo, location selection, and Celsius weather.
  The board requests fresh weather after a location change and every five minutes.
  Changing weather location does not change the configured clock timezone.
- Separate endpoint, key, model, and API-format settings for **LM Studio / Custom**,
  **OpenAI**, and **OpenCode**. Chat Completions and Responses formats are supported.
  A short provider test checks an actual model response.
- TTS and STT server configuration. TTS accepts a server root, a full
  `/v1/audio/speech` endpoint, or a legacy `/tts` endpoint. Model and voice are
  optional where the server supports its resident defaults. The TTS test actually
  synthesizes speech, validates WAV audio, and plays it on the board. A responding
  web interface alone does not count as a successful test. The speech server must
  have a usable TTS model loaded or selected.
- Camera video/audio controls, capture resolution, stream frame rate, and JPEG quality.
- Speaker volume, brightness, and power saving.

OpenCode uses the hosted API, with an editable endpoint. Its preset is
`https://opencode.ai/inference/openai/v1`. Cloud providers need their own API keys
and an appropriate model. OpenCode's free chat tier is restricted to OpenCode
clients; other clients use paid or Go access. Provider reference documentation:
[OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create),
[OpenCode API base URLs](https://opencode.ai/docs/console/harnesses#base-urls).
A local OpenCode application's native session API is not the hosted OpenCode API.

## Camera and microphone sharing

The camera is probed at boot on SCCB GPIO12/13, address 0x30, and then identified by
its sensor driver. If missing, the dashboard continues normally and Camera shows
**Camera unavailable** with its controls disabled. Reboot after reconnecting the
camera to detect it again.

Available SC2336 capture resolutions:

- 640 × 480
- 800 × 800
- 1024 × 600 (default)
- 1280 × 720
- 1920 × 1080

Preview and all network viewers share **one camera capture pipeline and one selected
resolution**. Frames are scaled to fit the board preview without stretching.
The web video stream uses hardware JPEG encoding and MJPEG over HTTP. Stream rate
is adjustable from 1–15 fps; actual delivery depends on Wi-Fi and resolution.
JPEG quality is adjustable from 20–90. At most two video viewers can connect.

The authenticated stream endpoints are:

```text
/stream       MJPEG video at the selected resolution
/audio.wav    Live 16 kHz mono PCM16 WAV microphone audio, when explicitly enabled
```

For example, `curl --user admin http://<board-ip>/stream -o camera.mjpeg` prompts
for the web password. Audio is a separate live stream; it is not muxed into the
MJPEG video. One audio listener can connect at a time.

The board microphone has explicit ownership. **The assistant has priority.** When
TALK starts recording, it takes the microphone after any current camera DMA read
finishes. Camera audio emits silence during assistant recording and resumes when
the assistant releases the microphone. Slow network clients cannot retain the mic
and block the assistant. Video continues independently.

Capture pauses when neither the board preview nor a network viewer needs it.
Leaving Camera or putting the display to sleep pauses only local preview; active
network video viewers keep capture running. Network recording to SD/flash is not
implemented.

`esp_video` is pinned to 1.4.1 because the mode switch uses that version's internal
sensor-format accessor. Revisit that accessor before changing the dependency.

## Spoken readiness confirmation

After connecting, Odin checks the selected model with a short inference request
and uses the configured TTS server to announce service readiness. If only TTS is
ready, it announces that service first. Failed checks retry at most every five
minutes. Successful announcements repeat only after reboot or provider/model/TTS
configuration changes. They wait until the assistant is idle and volume is above
zero. Press TALK during the spoken announcement to cancel playback.

## Board controls

Hold **TALK** while speaking; release or slide off to stop recording and send it.
Silence does not stop a held recording. A 30-second safety limit applies. Pressing
TALK during speech playback cancels the reply.

Power saving dims the screen to 60% of the configured brightness. After five
minutes without touch, the backlight turns off. The first touch restores brightness
and is consumed so that it does not also operate a button underneath.

Battery percentage and voltage come from the STC8 power MCU on the touch I2C bus
(GPIO45/46, address 0x2F), using byte-by-byte register reads every ten seconds.
Readings above 4.3 V display USB power and voltage. Invalid readings show unavailable.
The amplifier shutdown signal on GPIO30 is active high: playback enables it low.

## Memory configuration

This board's SDIO transport DMA buffers must remain in internal RAM:
`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` is disabled. Moving these buffers to PSRAM
caused SDIO transmit errors and resets on the tested board. Supported ESP-Hosted
task stacks, general large allocations, and TLS buffers use PSRAM instead, retaining
internal RAM for the display, network transport, task control blocks, and DMA.
The existing TCM reservation keeps invalid TCM stacks out of FreeRTOS allocations.

## Verification

See [tests/README.md](tests/README.md). Tests include WAV validation, concurrent
microphone ownership/preemption, and a live-board web/API/camera suite. Cloud
provider calls require user-supplied credentials; local inference and API request
handling can be tested independently.

The project configuration and flashed firmware can contain private credentials.
Keep project archives private.
