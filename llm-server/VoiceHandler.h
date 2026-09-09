#pragma once

struct HttpContext;

// /api/tts (SSE 音频流) + /api/asr (wav->文本) — 需登录
// 逻辑走 service/VoiceService (本地 piper / whisper.cpp)
class VoiceHandler {
public:
    static void handle(HttpContext& ctx);
};