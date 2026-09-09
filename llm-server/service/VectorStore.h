#pragma once

#include <string>
#include <vector>

// 检索命中
struct VecHit {
    long long itemId = 0;    // kb=chunk_id, msg=messages.id
    long long groupId = 0;   // kb=doc_id,   msg=conv_id
    float score = 0.f;       // 余弦(向量已归一化 -> 内积)
    std::string content;
};

// 进程内暴力余弦向量索引 (零依赖; 等价 IndexFlatIP 语义)
// - 向量存 MySQL vec_index(kind, group_id, item_id, content, vec BLOB float32)
// - 已选型 faiss, 本实现为该接口的 v0 后端; 后续可替换为 faiss 而不改动调用方
// - 个人规模(千级向量)暴力检索足够
class VectorStore {
public:
    // kind: "kb"(文档chunk) | "msg"(历史消息); vec 应为归一化向量
    static bool add(const std::string& kind, long long groupId, long long itemId,
                    const std::string& content, const std::vector<float>& vec);
    // top-k 检索: groups 非空则限定这些 group (kb=该用户文档, msg=单个会话)
    static std::vector<VecHit> topK(const std::string& kind,
                                    const std::vector<long long>& groups,
                                    const std::vector<float>& queryVec, int k);
    static bool clearGroup(const std::string& kind, long long groupId);

    // vec_state 游标 (增量回填用; 仅 msg 使用)
    static bool getState(const std::string& kind, long long groupId, long long& lastItemId);
    static void setState(const std::string& kind, long long groupId, long long lastItemId);

private:
    static std::string blobOf(const std::vector<float>& v);
    static std::vector<float> vecOfBlob(const std::string& b);
};