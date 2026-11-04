# Odin panel/backend protocol

Backend URL on the LAN: `http://<WINDOWS_PC_IP>:8000`

WebSocket:
`ws://<WINDOWS_PC_IP>:8000/api/v1/ws`

## Text chat request

```json
{"type":"chat","message":"Hello Odin","mode":"local"}
```

Reply:
```json
{"type":"assistant_text","text":"Hello! How can I help?","provider":"local"}
```

State messages:
```json
{"type":"state","state":"thinking"}
{"type":"state","state":"ready"}
```

## Camera upload

Send an HTTP `multipart/form-data` POST to `/api/v1/vision`:
- `question`: text prompt
- `image`: JPEG/PNG/WebP bytes

The board's existing V1.0 CSI camera code must capture and encode the frame. Do not add camera GPIO initialization from this bundle.

## Audio (next milestone; not implemented in this starter)

The intended future binary WebSocket frame format is 16 kHz, mono, signed 16-bit little-endian PCM. Agree a framing header/sequence number and sample format before implementing both endpoints. Current WebSocket endpoint handles JSON text events only.
