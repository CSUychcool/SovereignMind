#include "AppConfig.h"
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>

AppConfig AppConfig::s_instance;

void AppConfig::initDefaultModels() {
    m_models = {
        {"ollama", "qwen2.5:7b-instruct-q4_K_M", "ollama-qwen2.5"},
        {"ollama", "qwen3:8b",                   "ollama-qwen3"},
        {"vllm",   "Qwen2.5-7B-Instruct-AWQ",    "vllm-awq"},
    };
}

const AppConfig& AppConfig::get() { return s_instance; }

bool AppConfig::load(const char* configFile) {
    AppConfig& c = s_instance;
    c.initDefaultModels();

    std::ifstream f(configFile);
    Json::Value root;
    Json::Reader reader;
    if (!f.is_open() || !reader.parse(f, root)) {
        fprintf(stderr, "[AppConfig] cannot open/parse: %s\n", configFile ? configFile : "(null)");
        return false;
    }

    // upstream_url 形如 http://127.0.0.1:11434
    std::string url = root.get("upstream_url", "http://127.0.0.1:11434").asString();
    size_t sp = url.find("://");
    if (sp != std::string::npos) {
        std::string rest = url.substr(sp + 3);
        size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            c.upstreamHost = rest.substr(0, colon);
            c.upstreamPort = atoi(rest.substr(colon + 1).c_str());
        } else {
            c.upstreamHost = rest;
            c.upstreamPort = 443;
        }
    }
    c.upstreamPath = root.get("upstream_path", c.upstreamPath).asString();
    c.model = root.get("model", c.model).asString();
    c.webRoot = root.get("web_root", "").asString();
    c.contextWindow = root.get("context_window", c.contextWindow).asInt();
    c.reserveOutputTokens = root.get("reserve_output_tokens", c.reserveOutputTokens).asInt();
    c.summaryMinNewMessages = root.get("summary_min_new_messages", c.summaryMinNewMessages).asInt();
    c.historyFetchLimit = root.get("history_fetch_limit", c.historyFetchLimit).asInt();
    c.compactionMode = root.get("compaction_mode", c.compactionMode).asString();
    c.maxCompressRounds = root.get("max_compress_rounds", c.maxCompressRounds).asInt();

    c.embedHost = root.get("embed_host", c.embedHost).asString();
    c.embedPort = root.get("embed_port", c.embedPort).asInt();
    c.embedModel = root.get("embed_model", c.embedModel).asString();
    c.embedDim = root.get("embed_dim", c.embedDim).asInt();
    c.embedTaskPrefix = root.get("embed_task_prefix", c.embedTaskPrefix).asBool();
    c.ragDefaultOn = root.get("rag_default_on", c.ragDefaultOn).asBool();
    c.graphDefaultOn = root.get("graph_default_on", c.graphDefaultOn).asBool();
    c.ragTopK = root.get("rag_top_k", c.ragTopK).asInt();
    c.ragTokenQuota = root.get("rag_token_quota", c.ragTokenQuota).asInt();
    c.msgRecallTopK = root.get("msg_recall_top_k", c.msgRecallTopK).asInt();
    c.recallMode = root.get("recall_mode", c.recallMode).asString();

    c.piperPath = root.get("piper_path", c.piperPath).asString();
    c.piperVoice = root.get("piper_voice", c.piperVoice).asString();
    c.piperVoiceCfg = root.get("piper_voice_cfg", c.piperVoiceCfg).asString();
    c.whisperPath = root.get("whisper_path", c.whisperPath).asString();
    c.whisperModel = root.get("whisper_model", c.whisperModel).asString();
    c.whisperLang = root.get("whisper_lang", c.whisperLang).asString();
    c.ttsLengthScale = root.get("tts_length_scale", c.ttsLengthScale).asDouble();
    c.ttsBackend = root.get("tts_backend", c.ttsBackend).asString();
    c.cosyHost = root.get("cosy_host", c.cosyHost).asString();
    c.cosyPort = root.get("cosy_port", c.cosyPort).asInt();

    // db 段 (可选; 缺失则 hasDb=false, 鉴权/会话接口不可用)
    const Json::Value& db = root["db"];
    if (db.isObject()) {
        c.hasDb = true;
        c.dbHost = db.get("host", "127.0.0.1").asString();
        c.dbPort = db.get("port", 3306).asInt();
        c.dbUser = db.get("user", "llm_chat").asString();
        c.dbPassword = db.get("password", "").asString();
        c.dbName = db.get("database", "llm_chat").asString();
    }
    return true;
}