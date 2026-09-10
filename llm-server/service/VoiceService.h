#pragma once

#include <string>
#include <vector>

class Response;

// V1 语音服务: 本地 TTS(piper) + 本地 ASR(whisper.cpp)
// 引擎/模型路径来自 AppConfig voice.*
class VoiceService {
public:
    // TTS: 文本按句切分, 每句调 piper 合成 wav, 以 SSE data:<JSON {s,b64}> 逐句推送, 结束 [DONE]
    static void streamTts(const std::string& text, double lengthScale, const std::string& voice,
                          Response& resp);

    // ASR: wav 字节 -> 文本; 失败返回 false
    static bool transcribe(const std::string& wavData, std::string& text);

private:
    static std::vector<std::string> splitSentences(const std::string& text);
    static std::string runPiper(const std::string& sentence, double lengthScale);
    static std::string runCosyVoice(const std::string& sentence, const std::string& voice);   // sidecar GET /tts
    static std::string synthSentence(const std::string& sentence, double lengthScale, const std::string& voice); // cosy 优先, piper 兜底
    static std::string base64Encode(const std::string& in);
    static std::string whisperBin();   // 实际可执行(兼容 whisper-cli/main 命名)
};