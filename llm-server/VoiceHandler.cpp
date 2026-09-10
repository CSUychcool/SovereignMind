#include "VoiceHandler.h"
#include "net/HttpContext.h"
#include "service/VoiceService.h"
#include "config/AppConfig.h"
#include <json/json.h>
#include <json/reader.h>
#include <algorithm>
#include <string>

using namespace std;

void VoiceHandler::handle(HttpContext& ctx) {
    // ---- TTS: POST /api/tts {text, rate?} -> SSE data:<{s,b64}> -> [DONE] ----
    if (ctx.method == "POST" && ctx.url == "/api/tts") {
        Json::Value req;
        Json::Reader reader;
        if (!reader.parse(string(ctx.body ? ctx.body : "", ctx.bodyLen), req)) {
            ctx.resp.sendErr("参数格式错误"); return;
        }
        string text = req.get("text", "").asString();
        if (text.empty()) { ctx.resp.sendErr("text 为空"); return; }
        string voice = req.get("voice", AppConfig::get().voiceDefault).asString();
        double rate = req.get("rate", 1.0).asDouble();
        rate = max(0.5, min(2.0, rate));                 // 前端调速 0.5x~2x
        double ls = 1.0 / rate;                          // piper length-scale
        VoiceService::streamTts(text, ls, voice, ctx.resp);  // 流式 SSE, 结束后由 Response 关闭 fd
        return;
    }

    // ---- ASR: POST /api/asr (body=wav 二进制) -> {code, text} ----
    if (ctx.method == "POST" && ctx.url == "/api/asr") {
        string wav(ctx.body ? ctx.body : "", ctx.bodyLen);
        string text;
        if (!VoiceService::transcribe(wav, text)) {
            ctx.resp.sendErr("识别失败(引擎可用? 波形格式 wav)"); return;
        }
        Json::Value resp;
        resp["code"] = 0;
        resp["text"] = text;
        ctx.resp.sendJson(200, "OK", resp);
        return;
    }

    ctx.resp.sendErr("unknown voice route");
}