#pragma once

#include <string>
#include <vector>

// Ollama /api/embed 客户端 (与聊天上游解耦: 常驻 embedHost:embedPort)
// - 输入文本批量 embed, 输出向量统一做 L2 归一化 (内积 == 余弦)
// - nomic 检索建议: 语料前缀 "search_document:", 提问前缀 "search_query:" (AppConfig::embedTaskPrefix)
class EmbedClient {
public:
    static bool embed(const std::string& text, bool asQuery, std::vector<float>& out);
    static bool embedBatch(const std::vector<std::string>& texts, bool asQuery,
                           std::vector<std::vector<float>>& out);
private:
    static void normalize(std::vector<float>& v);
    static std::string decodeChunked(const std::string& s);
};