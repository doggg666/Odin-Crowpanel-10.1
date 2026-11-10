# Verification

From the project root, run the host tests:

```sh
gcc -B/usr/bin/ -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I main main/wav.c tests/test_wav.c -o /tmp/odin-test-wav
ASAN_OPTIONS=detect_leaks=0 /tmp/odin-test-wav

gcc -B/usr/bin/ -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -pthread -I tests/host -I main main/microphone.c tests/test_microphone.c \
    -o /tmp/odin-test-microphone
ASAN_OPTIONS=detect_leaks=0 /tmp/odin-test-microphone
```

`-B/usr/bin/` selects the host assembler/linker when the ESP-IDF toolchain is on
PATH. Leak detection is disabled for environments running under ptrace; address
and undefined-behavior checks remain enabled.

The WAV suite covers supported formats, truncation, invalid RIFF/chunk bounds,
unsupported encodings, invalid channel/rate/alignment values, and padded chunks.
The microphone suite uses pthread-backed FreeRTOS mocks and checks actual owner
transitions, assistant preemption during an in-flight DMA read, wrong-owner release,
camera resumption, and acquisition failures. Assertions in the fake hardware detect
concurrent reads or stopping DMA during a read.

Build the firmware with `idf.py build`. With a board running on the local network:

```sh
read -s ODIN_WEB_PASSWORD
export ODIN_WEB_PASSWORD
python3 tests/test_web_board.py --url http://<board-ip>
unset ODIN_WEB_PASSWORD
```

The live suite checks HTTP authentication, secret redaction, rejection of invalid
settings without partial writes, Wi-Fi scanning, country/city search, settings
round trips, provider inference, real TTS synthesis/playback, all camera resolution
streams, concurrent video/audio, listener limits, resource cleanup, and uptime.
It temporarily changes camera settings and restores them afterward. It makes a
short provider request and plays test speech through the board speaker.

Use `--no-speech` if no usable provider/TTS model is loaded; use `--no-streams` when
testing without a camera. These options do not validate the skipped features.
Wi-Fi reconnection and a physical TALK test while camera audio streams are separate
hardware checks. Reconnect to the board's setup AP if changing Wi-Fi disconnects
your browser. A missing-camera boot path can be verified by booting without the
camera attached; the rest of the dashboard must continue normally.
