#pragma once

#include <string>
#include <vector>

// 单个可切换模型条目 (backend/model -> start_model.sh 参数)
struct ModelEntry {
    std::string backend;
    std::string model;
    std::string choice;
};

// 全局配置单例: 统一解析 config json 一次
// (upstream / db / web_root / 模型控制), 全项目从此处取值
class AppConfig {
public:
    // 解析并装载 config 文件 (可带默认值, 失败返回 false)
    static bool load(const char* configFile);
    static const AppConfig& get();

    // ---- 服务 (CLI --port / --threads 可覆盖) ----
    unsigned short port = 9000;
    int threads = 4;

    // ---- 上游模型 (Ollama 11434 / vLLM 8000) ----
    std::string upstreamHost = "127.0.0.1";
    int upstreamPort = 11434;
    std::string upstreamPath = "/v1/chat/completions";
    std::string model = "qwen2.5:7b-instruct-q4_K_M";
    std::string webRoot;            // 静态页面根目录 (空=不提供网页)

    // ---- MySQL ----
    bool hasDb = false;
    std::string dbHost = "127.0.0.1";
    int dbPort = 3306;
    std::string dbUser = "llm_chat";
    std::string dbPassword;
    std::string dbName = "llm_chat";

    // ---- 模型控制 (切换脚本与可切换组合) ----
    std::string startScript = "/home/yc_21/server_ddz/llm-server/start_model.sh";
    const std::vector<ModelEntry>& models() const { return m_models; }

    // ---- 上下文管理 ----
    int contextWindow = 32768;        // 模型窗口 (Ollama 需 OLLAMA_CONTEXT_LENGTH 同步)
    int reserveOutputTokens = 6000;   // 预留给模型输出
    int summaryMinNewMessages = 20;   // P1: 距上次摘要新增多少条消息才触发重新压缩 (throttled 模式)
    int historyFetchLimit = 500;      // 单次从 DB 拉取的历史上限件数(再按预算裁剪)
    // 滚动压缩模式(P1 方案B落地):
    //   "until_fit"(默认): 溢出时单请求内循环连压, 直到未压缩活跃段可装进预算(hasOlder=false)或达 maxCompressRounds
    //   "throttled"      : 旧方案, 新增>=summaryMinNewMessages 才触发, 每请求最多一轮
    std::string compactionMode = "until_fit";
    int maxCompressRounds = 5;        // until_fit 单请求最多压缩轮数(防死循环/防小预算卡死)

    // ---- P1 embedding / RAG / 历史召回向量化 ----
    std::string embedHost = "127.0.0.1";
    int embedPort = 11434;            // 与聊天上游解耦: 固定 Ollama /api/embed
    std::string embedModel = "nomic-embed-text";
    int embedDim = 768;               // nomic-embed-text 输出维度
    bool embedTaskPrefix = true;      // nomic 检索建议: 语料加 "search_document:", 提问加 "search_query:"
    // RAG
    bool ragDefaultOn = true;         // use_rag 请求默认 (无知识库时自动跳过)
    bool graphDefaultOn = false;      // use_graph 请求默认 (构图固定做, 仅默认不用)
    int ragTopK = 5;                  // 每次检索注入的 chunk 数
    int ragTokenQuota = 512;          // 参考资料段注入预算上限(token)
    // 历史会话召回
    int msgRecallTopK = 6;            // 历史会话向量召回条数
    std::string recallMode = "vector";// "vector"(向量优先, LIKE 兜底) | "like"(仅旧 LIKE 路径)

    // ---- V1 语音引擎 (CosyVoice2 sidecar 优先 + piper 兜底; whisper.cpp ASR) ----
    std::string ttsBackend = "cosyvoice";   // "cosyvoice"(常驻GPU sidecar) | "piper"(本地onnx)
    std::string cosyHost = "127.0.0.1";
    int cosyPort = 9101;                    // voice/cosyvoice_server.py
    std::string piperPath = "/home/yc_21/server_ddz/voice/piper/piper/piper";
    std::string piperVoice = "/home/yc_21/server_ddz/voice/piper-voices/zh_CN-huayan-medium.onnx";
    std::string piperVoiceCfg = "/home/yc_21/server_ddz/voice/piper-voices/zh_CN-huayan-medium.onnx.json";
    std::string whisperPath = "/home/yc_21/server_ddz/voice/whisper.cpp/build/bin/whisper-cli";
    std::string whisperModel = "/home/yc_21/server_ddz/voice/whisper-models/ggml-base.bin";
    std::string whisperLang = "zh";       // ASR 强制语言(本应用中文场景; 避免 base 模型误判语种)
    std::string voiceDefault = "linzhi";    // 默认 TTS 音色名(CosyVoice 零样本库 voice/cosyvoice-voices)
    double ttsLengthScale = 1.0;      // piper 基准语速 (>1 慢, <1 快); 浏览器端另有 playbackRate 调速
    // 可行历史 token 预算 = 窗口 - 输出预留 (若被前端/系统提示覆盖则动态减少)
    int usableHistoryTokens() const { return contextWindow - reserveOutputTokens; }

private:
    void initDefaultModels();
    std::vector<ModelEntry> m_models;
    static AppConfig s_instance;
};