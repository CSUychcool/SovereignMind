#pragma once

#include <string>
#include <vector>
#include "ConversationService.h"   // MsgRow

struct KbDocRow {
    long long id = 0;
    std::string title;
    std::string filename;
    int chunkCount = 0;
    std::string status;
    std::string createdAt;
};

struct KbHit {
    long long docId = 0;
    int idx = 0;
    std::string content;
};

// 知识库领域 (P1 需求二): 文档上传/切块/embed/入库/GRAPH 抽取/检索
// + P1-C 历史会话消息向量化与检索
class KnowledgeService {
public:
    // ---- 知识库 (txt/md v0) ----
    static bool uploadDocument(long long uid, const std::string& title,
                               const std::string& filename, const std::string& content);
    static bool listDocuments(long long uid, std::vector<KbDocRow>& out);
    static bool removeDocument(long long uid, long long docId);
    static long long userDocCount(long long uid);

    // ---- 检索注入 ----
    static bool retrieveHits(long long uid, const std::string& query, int topK,
                             std::vector<KbHit>& hits);
    // 图邻域文本 (entities + 1-hop edges), 命中 doc 集合; 空=无图内容
    static std::string graphContext(long long uid, const std::vector<long long>& docIds, int capLines);

    // ---- P1-C 历史会话向量化 ----
    static void ensureMsgVectors(long long convId);   // 增量回填 (vec_state 游标)
    static void msgVectorSearch(long long convId, const std::vector<float>& qvec, int k,
                                std::vector<MsgRow>& out);

private:
    static std::vector<std::string> chunkText(const std::string& text);
    static bool extractGraph(long long docId, const std::string& joinedContent);
};