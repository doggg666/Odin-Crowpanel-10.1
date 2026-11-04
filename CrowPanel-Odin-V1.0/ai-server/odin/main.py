from __future__ import annotations

import base64
import logging
from typing import Literal

from fastapi import FastAPI, File, Form, HTTPException, UploadFile, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, Field
from ollama import Client as OllamaClient
from openai import OpenAI

from . import config

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("odin")

app = FastAPI(title="Odin AI Companion API", version="0.1.0")
ollama = OllamaClient(host=config.OLLAMA_HOST)

Mode = Literal["local", "cloud", "auto"]


class ChatRequest(BaseModel):
    message: str = Field(min_length=1, max_length=12000)
    mode: Mode | None = None
    history: list[dict[str, str]] = Field(default_factory=list)


def local_chat(message: str, history: list[dict[str, str]]):
    messages = [{"role": "system", "content": config.SYSTEM_PROMPT}]
    for item in history[-12:]:
        role = item.get("role")
        content = item.get("content")
        if role in ("user", "assistant") and isinstance(content, str):
            messages.append({"role": role, "content": content[:12000]})
    messages.append({"role": "user", "content": message})
    result = ollama.chat(model=config.OLLAMA_CHAT_MODEL, messages=messages)
    return result.message.content


def cloud_chat(message: str, history: list[dict[str, str]]):
    if not config.CLOUD_API_KEY or not config.CLOUD_MODEL:
        raise RuntimeError("Cloud mode is not configured. Set CLOUD_API_KEY and CLOUD_MODEL in .env.")
    client = OpenAI(api_key=config.CLOUD_API_KEY, base_url=config.CLOUD_BASE_URL)
    messages = [{"role": "system", "content": config.SYSTEM_PROMPT}]
    for item in history[-12:]:
        role, content = item.get("role"), item.get("content")
        if role in ("user", "assistant") and isinstance(content, str):
            messages.append({"role": role, "content": content[:12000]})
    messages.append({"role": "user", "content": message})
    result = client.chat.completions.create(model=config.CLOUD_MODEL, messages=messages)
    return result.choices[0].message.content or ""


def route_chat(message: str, history: list[dict[str, str]], mode: Mode):
    if mode == "local":
        return local_chat(message, history), "local"
    if mode == "cloud":
        return cloud_chat(message, history), "cloud"
    try:
        return local_chat(message, history), "local"
    except Exception as exc:
        log.warning("Local model failed; trying configured cloud fallback: %s", exc)
        if config.CLOUD_API_KEY and config.CLOUD_MODEL:
            return cloud_chat(message, history), "cloud"
        raise


@app.get("/api/v1/health")
def health():
    return {
        "status": "ok",
        "assistant": "Odin",
        "configured_mode": config.AI_MODE,
        "ollama_host": config.OLLAMA_HOST,
        "chat_model": config.OLLAMA_CHAT_MODEL,
        "vision_model": config.OLLAMA_VISION_MODEL,
    }


@app.post("/api/v1/chat")
def chat_endpoint(req: ChatRequest):
    mode: Mode = req.mode or config.AI_MODE  # type: ignore[assignment]
    if mode not in ("local", "cloud", "auto"):
        raise HTTPException(400, "mode must be local, cloud, or auto")
    try:
        reply, provider = route_chat(req.message, req.history, mode)
        return {"assistant": "Odin", "reply": reply, "provider": provider}
    except Exception as exc:
        log.exception("Chat request failed")
        raise HTTPException(502, f"AI request failed: {exc}") from exc


@app.post("/api/v1/vision")
async def vision(
    question: str = Form("Describe this image."),
    image: UploadFile = File(...),
):
    if image.content_type not in ("image/jpeg", "image/png", "image/webp"):
        raise HTTPException(415, "Upload a JPEG, PNG, or WebP image.")
    raw = await image.read(config.MAX_IMAGE_BYTES + 1)
    if len(raw) > config.MAX_IMAGE_BYTES:
        raise HTTPException(413, "Image exceeds MAX_IMAGE_BYTES.")
    try:
        result = ollama.chat(
            model=config.OLLAMA_VISION_MODEL,
            messages=[{
                "role": "user",
                "content": question,
                "images": [base64.b64encode(raw).decode("ascii")],
            }],
        )
        return {"assistant": "Odin", "reply": result.message.content, "provider": "local"}
    except Exception as exc:
        log.exception("Vision request failed")
        raise HTTPException(502, f"Local vision request failed: {exc}") from exc


@app.websocket("/api/v1/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    await ws.send_json({"type": "state", "state": "ready", "assistant": "Odin"})
    try:
        while True:
            data = await ws.receive_json()
            event_type = data.get("type")
            if event_type == "ping":
                await ws.send_json({"type": "pong"})
                continue
            if event_type == "chat":
                message = str(data.get("message", "")).strip()
                mode = data.get("mode") or config.AI_MODE
                if not message:
                    await ws.send_json({"type": "error", "message": "Empty message"})
                    continue
                await ws.send_json({"type": "state", "state": "thinking"})
                try:
                    reply, provider = route_chat(message, data.get("history", []), mode)
                    await ws.send_json({
                        "type": "assistant_text",
                        "text": reply,
                        "provider": provider,
                    })
                    await ws.send_json({"type": "state", "state": "ready"})
                except Exception as exc:
                    log.exception("WebSocket chat failed")
                    await ws.send_json({"type": "error", "message": str(exc)})
            elif event_type == "interrupt":
                await ws.send_json({"type": "state", "state": "ready"})
            else:
                await ws.send_json({"type": "error", "message": f"Unsupported event type: {event_type}"})
    except WebSocketDisconnect:
        log.info("Panel disconnected")
