#pragma once

#include <string>
#include <json/json.h>

class Response;

// 上游大模型网关: 连接 Ollama/vLLM, 转发 OpenAI 风格请求。
// - chatStream: 把 SSE 流式回传给客户端并累积完整回复 (chat 用)
// - summarize: 非流式归档调用, 仅取完整文本 (P1 滚动压缩用)
// 请求体组装(messages)在上层 Handler; 网关只做"传输+解析"。
class LlmGateway {
public:
    // chatStream: 把 SSE 流式回传给客户端并累积完整回复 (chat 用)
    // contextEvent 非空时, 在 [DONE] 前追加一条 data:<context json> (上下文透明面板)
    static bool chatStream(const Json::Value& openaiReq, Response& resp, std::string& aiFull,
                           const Json::Value* contextEvent = nullptr);
    static bool summarize(const Json::Value& openaiReq, std::string& outText);

private:
    // resp == nullptr 时不回写 SSE (accumulate only)
    static bool relay(const Json::Value& openaiReq, Response* resp, std::string& out,
                      const Json::Value* ctxEvent = nullptr);
};