#include "KnowledgeService.h"
#include "EmbedClient.h"
#include "VectorStore.h"
#include "LlmGateway.h"
#include "Db.h"
#include "Log.h"
#include "config/AppConfig.h"
#include <json/json.h>
#include <json/reader.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>

using namespace std;

// ---------- 切块 (v0 启发式: 段落累积 512 字符 + 尾 64 字符重叠) ----------
vector<string> KnowledgeService::chunkText(const string& text) {
    const int target = 512, overlap = 64;
    vector<string> paras;
    string line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
            if (!line.empty()) paras.push_back(line);
            line.clear();
        } else {
            line.push_back(text[i]);
        }
    }

    vector<string> out;
    string chunk;
    for (auto& p : paras) {
        // 单段过长: 直接切段 (带 overlap)
        while (p.size() > (size_t)target) {
            if (!chunk.empty()) { out.push_back(chunk); chunk.clear(); }
            out.push_back(p.substr(0, (size_t)target));
            p = p.substr((size_t)target - (size_t)overlap);
        }
        if (!chunk.empty() && chunk.size() + p.size() + 1 > (size_t)target) {
            out.push_back(chunk);
            chunk = chunk.size() > (size_t)overlap ? chunk.substr(chunk.size() - (size_t)overlap) : "";
        }
        if (!chunk.empty()) chunk += "\n";
        chunk += p;
    }
    if (!chunk.empty()) out.push_back(chunk);
    return out;
}

// ---------- 知识库 ----------
bool KnowledgeService::uploadDocument(long long uid, const string& title,
                                      const string& filename, const string& content) {
    // v0 仅 txt/md
    string lower = filename;
    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    bool okFmt = lower.size() > 4 && lower.substr(lower.size() - 4) == ".txt" ||
                 (lower.size() > 3 && lower.substr(lower.size() - 3) == ".md");
    if (!okFmt) {
        tprintf("[KB] unsupported format: %s (v0: txt/md)\n", filename.c_str());
        return false;
    }
    if (content.empty()) return false;

    string sql = "INSERT INTO kb_documents(user_id, title, filename) VALUES(" +
                 to_string(uid) + ",'" + Db::escape(title) + "','" + Db::escape(filename) + "')";
    long long docId = Db::insert(sql);
    if (docId < 0) return false;

    vector<string> chunks = chunkText(content);
    vector<vector<float>> vecs;
    if (chunks.empty() || !EmbedClient::embedBatch(chunks, /*asQuery=*/false, vecs) ||
        vecs.size() != chunks.size()) {
        Db::update("UPDATE kb_documents SET status='failed' WHERE id=" + to_string(docId));
        tprintf("[KB] doc %lld embed failed (%zu chunks)\n", docId, chunks.size());
        return false;
    }

    for (size_t i = 0; i < chunks.size(); ++i) {
        long long chunkId = Db::insert("INSERT INTO kb_chunks(doc_id, idx, content) VALUES(" +
                                       to_string(docId) + "," + to_string((int)i) + ",'" +
                                       Db::escape(chunks[i]) + "')");
        if (chunkId < 0) continue;
        VectorStore::add("kb", docId, chunkId, chunks[i], vecs[i]);
    }
    Db::update("UPDATE kb_documents SET status='ready', chunk_count=" +
               to_string((int)chunks.size()) + " WHERE id=" + to_string(docId));

    string joined;
    for (auto& c : chunks) joined += c + "\n";
    extractGraph(docId, joined);   // 构图固定生成, 失败不影响入库

    tprintf("[KB] doc %lld '%s' ready: %zu chunks\n", docId, filename.c_str(), chunks.size());
    fflush(stdout);
    return true;
}

bool KnowledgeService::listDocuments(long long uid, vector<KbDocRow>& out) {
    if (!Db::query("SELECT id, title, filename, chunk_count, status, created_at FROM kb_documents "
                   "WHERE user_id=" + to_string(uid) + " ORDER BY id DESC")) return false;
    while (Db::next()) {
        KbDocRow r;
        r.id = atoll(Db::value(0).c_str());
        r.title = Db::value(1);
        r.filename = Db::value(2);
        r.chunkCount = atoi(Db::value(3).c_str());
        r.status = Db::value(4);
        r.createdAt = Db::value(5);
        out.push_back(r);
    }
    return true;
}

bool KnowledgeService::removeDocument(long long uid, long long docId) {
    if (!Db::query("SELECT id FROM kb_documents WHERE id=" + to_string(docId) +
                   " AND user_id=" + to_string(uid))) return false;
    if (!Db::next()) return false;
    // 级联删除 chunks/vecs/entities/edges + 文档
    Db::update("DELETE FROM kb_edges WHERE doc_id=" + to_string(docId));
    Db::update("DELETE FROM kb_entities WHERE doc_id=" + to_string(docId));
    Db::update("DELETE FROM kb_chunks WHERE doc_id=" + to_string(docId));
    VectorStore::clearGroup("kb", docId);
    Db::update("DELETE FROM kb_documents WHERE id=" + to_string(docId));
    return true;
}

long long KnowledgeService::userDocCount(long long uid) {
    if (!Db::query("SELECT COUNT(*) FROM kb_documents WHERE user_id=" + to_string(uid) +
                   " AND status='ready'")) return 0;
    return Db::next() ? atoll(Db::value(0).c_str()) : 0;
}

// ---------- 检索注入 ----------
bool KnowledgeService::retrieveHits(long long uid, const string& query, int topK,
                                    vector<KbHit>& hits) {
    if (topK <= 0) return false;
    if (userDocCount(uid) <= 0) return false;

    // 该用户 ready 文档 id 集合
    vector<long long> docIds;
    if (!Db::query("SELECT id FROM kb_documents WHERE user_id=" + to_string(uid) +
                   " AND status='ready'")) return false;
    while (Db::next()) docIds.push_back(atoll(Db::value(0).c_str()));
    if (docIds.empty()) return false;

    vector<float> qvec;
    if (!EmbedClient::embed(query, /*asQuery=*/true, qvec)) return false;

    vector<VecHit> top = VectorStore::topK("kb", docIds, qvec, topK);
    if (top.empty()) return false;

    // 补 idx
    string in;
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) in += ",";
        in += to_string(top[i].itemId);
    }
    if (Db::query("SELECT id, doc_id, idx FROM kb_chunks WHERE id IN (" + in + ")")) {
        while (Db::next()) {
            long long cid = atoll(Db::value(0).c_str());
            long long did = atoll(Db::value(1).c_str());
            int idx = atoi(Db::value(2).c_str());
            for (auto& t : top) {
                if (t.itemId == cid) {
                    KbHit h;
                    h.docId = did;
                    h.idx = idx;
                    h.content = t.content;
                    hits.push_back(h);
                    break;
                }
            }
        }
    }
    return !hits.empty();
}

string KnowledgeService::graphContext(long long uid, const vector<long long>& docIds, int capLines) {
    if (docIds.empty()) return "";
    string in;
    for (size_t i = 0; i < docIds.size(); ++i) {
        if (i) in += ",";
        in += to_string(docIds[i]);
    }
    string lines;
    int n = 0;
    if (Db::query("SELECT e.name, e.type FROM kb_entities e JOIN kb_documents d ON e.doc_id=d.id "
                  "WHERE e.doc_id IN (" + in + ") AND d.user_id=" + to_string(uid) + " LIMIT 40")) {
        while (Db::next() && n < capLines) {
            lines += "ENT: " + Db::value(0) + (Db::value(1).empty() ? "" : "(" + Db::value(1) + ")") + "\n";
            ++n;
        }
    }
    if (Db::query("SELECT a.name, b.name, r.relation FROM kb_edges r "
                  "JOIN kb_entities a ON r.from_entity_id=a.id "
                  "JOIN kb_entities b ON r.to_entity_id=b.id "
                  "JOIN kb_documents d ON r.doc_id=d.id "
                  "WHERE r.doc_id IN (" + in + ") AND d.user_id=" + to_string(uid) +
                  " LIMIT 60")) {
        while (Db::next() && n < capLines) {
            lines += "REL: " + Db::value(0) + " -[" + Db::value(2) + "]-> " + Db::value(1) + "\n";
            ++n;
        }
    }
    return lines;
}

// ---------- P1-C 历史会话向量化 ----------
void KnowledgeService::ensureMsgVectors(long long convId) {
    long long last = 0;
    VectorStore::getState("msg", convId, last);
    string sql = "SELECT id, content FROM messages WHERE conv_id=" + to_string(convId) +
                 " AND id>" + to_string(last) + " ORDER BY id ASC LIMIT 2000";
    if (!Db::query(sql)) return;
    vector<long long> ids;
    vector<string> contents;
    while (Db::next()) {
        ids.push_back(atoll(Db::value(0).c_str()));
        contents.push_back(Db::value(1));
    }
    if (ids.empty()) return;

    vector<vector<float>> vecs;
    if (!EmbedClient::embedBatch(contents, /*asQuery=*/false, vecs) || vecs.size() != ids.size()) {
        tprintf("[KB] ensureMsgVectors embed failed (conv %lld)\n", convId);
        return;
    }
    for (size_t i = 0; i < ids.size(); ++i)
        VectorStore::add("msg", convId, ids[i], contents[i], vecs[i]);
    VectorStore::setState("msg", convId, ids.back());
    tprintf("[KB] msg vectors %zu added (conv %lld, upto id %lld)\n",
            ids.size(), convId, ids.back());
    fflush(stdout);
}

void KnowledgeService::msgVectorSearch(long long convId, const vector<float>& qvec, int k,
                                       vector<MsgRow>& out) {
    vector<VecHit> top = VectorStore::topK("msg", {convId}, qvec, k);
    if (top.empty()) return;
    string in;
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) in += ",";
        in += to_string(top[i].itemId);
    }
    if (Db::query("SELECT id, role, content FROM messages WHERE id IN (" + in + ") ORDER BY id ASC")) {
        while (Db::next()) {
            MsgRow r;
            r.role = Db::value(1);
            r.content = Db::value(2);
            out.push_back(r);
        }
    }
}

// ---------- GRAPH 抽取 (LLM, 固定生成) ----------
bool KnowledgeService::extractGraph(long long docId, const string& joinedContent) {
    if (joinedContent.size() > 3000) return false;   // 素材截断保护
    Json::Value req;
    req["model"] = AppConfig::get().model;
    req["stream"] = true;
    Json::Value messages(Json::arrayValue);
    Json::Value sys;
    sys["role"] = "system";
    sys["content"] = "你是知识图谱抽取器。从文档片段中抽取重要实体与它们之间的关系。"
                     "只输出 JSON，不要任何解释。格式: {\"entities\":[{\"name\":\"\",\"type\":\"\"}],"
                     "\"relations\":[{\"from\":\"\",\"to\":\"\",\"relation\":\"\"}]}";
    messages.append(sys);
    Json::Value usr;
    usr["role"] = "user";
    usr["content"] = "文档片段:\n" + joinedContent;
    messages.append(usr);
    req["messages"] = messages;

    string text;
    if (!LlmGateway::summarize(req, text) || text.empty()) return false;

    // 容错包裹 { } 再解析
    std::string maybe = text;
    size_t b = maybe.find('{'), e = maybe.rfind('}');
    if (b == string::npos || e == string::npos || e <= b) return false;
    Json::Value root;
    Json::Reader rd;
    if (!rd.parse(maybe.substr(b, e - b + 1), root)) return false;

    const Json::Value& ents = root["entities"];
    vector<long long> entIds;
    if (ents.isArray()) {
        int cap = min<int>((int)ents.size(), 40);
        for (int i = 0; i < cap; ++i) {
            string name = ents[i].get("name", "").asString();
            string type = ents[i].get("type", "").asString();
            if (name.empty()) continue;
            long long id = Db::insert("INSERT INTO kb_entities(doc_id, name, type) VALUES(" +
                                      to_string(docId) + ",'" + Db::escape(name) + "','" +
                                      Db::escape(type) + "')");
            if (id > 0) entIds.push_back(id);
        }
    }
    const Json::Value& rels = root["relations"];
    if (rels.isArray()) {
        int cap = min<int>((int)rels.size(), 40);
        for (int i = 0; i < cap; ++i) {
            string a = rels[i].get("from", "").asString();
            string b = rels[i].get("to", "").asString();
            string rel = rels[i].get("relation", "").asString();
            if (a.empty() || b.empty()) continue;
            // 名称 -> 已入库实体 id 的近似匹配 (仅当存在同名实体时保留边)
            long long aid = -1, bid = -1;
            if (Db::query("SELECT id FROM kb_entities WHERE doc_id=" + to_string(docId) +
                          " AND name='" + Db::escape(a) + "' LIMIT 1") && Db::next()) aid = atoll(Db::value(0).c_str());
            if (Db::query("SELECT id FROM kb_entities WHERE doc_id=" + to_string(docId) +
                          " AND name='" + Db::escape(b) + "' LIMIT 1") && Db::next()) bid = atoll(Db::value(0).c_str());
            if (aid < 0 || bid < 0) continue;
            Db::update("INSERT INTO kb_edges(doc_id, from_entity_id, to_entity_id, relation) VALUES(" +
                       to_string(docId) + "," + to_string(aid) + "," + to_string(bid) + ",'" +
                       Db::escape(rel) + "')");
        }
    }
    tprintf("[KB] graph extracted: %zu entities, %zu relations (doc %lld)\n",
            entIds.size(), rels.isArray() ? (size_t)rels.size() : 0, docId);
    fflush(stdout);
    return true;
}