import os
from dotenv import load_dotenv

load_dotenv()


def env(name: str, default: str = "") -> str:
    return os.getenv(name, default)


AI_MODE = env("AI_MODE", "local").lower()
LOCAL_BACKEND = env("LOCAL_BACKEND", "ollama").lower()
OLLAMA_HOST = env("OLLAMA_HOST", "http://127.0.0.1:11434")
OLLAMA_CHAT_MODEL = env("OLLAMA_CHAT_MODEL", "qwen3:8b")
OLLAMA_VISION_MODEL = env("OLLAMA_VISION_MODEL", "qwen2.5vl:7b")
LM_STUDIO_BASE_URL = env("LM_STUDIO_BASE_URL", "http://127.0.0.1:1234/v1")
LM_STUDIO_API_KEY = env("LM_STUDIO_API_KEY", "lm-studio")
LM_STUDIO_MODEL = env("LM_STUDIO_MODEL", "")
CLOUD_BASE_URL = env("CLOUD_BASE_URL", "https://api.openai.com/v1")
CLOUD_API_KEY = env("CLOUD_API_KEY")
CLOUD_MODEL = env("CLOUD_MODEL")
SYSTEM_PROMPT = env(
    "ODIN_SYSTEM_PROMPT",
    "You are Odin, a friendly, capable voice assistant. Be concise and natural when speaking."
)
MAX_IMAGE_BYTES = int(env("MAX_IMAGE_BYTES", "12000000"))
