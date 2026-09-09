#include "VectorStore.h"
#include "Db.h"
#include "Log.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

std::string VectorStore::blobOf(const std::vector<float>& v) {
    if (v.empty()) return "";
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
}

std::vector<float> VectorStore::vecOfBlob(const std::string& b) {
    std::vector<float> v;
    if (b.size() < sizeof(float) || b.size() % sizeof(float) != 0) return v;
    v.resize(b.size() / sizeof(float));
    memcpy(v.data(), b.data(), b.size());
    return v;
}

bool VectorStore::add(const std::string& kind, long long groupId, long long itemId,
                      const std::string& content, const std::vector<float>& vec) {
    if (kind != "kb" && kind != "msg") return false;
    std::string sql = "INSERT INTO vec_index(kind, group_id, item_id, content, vec) VALUES('" +
                      kind + "'," + std::to_string(groupId) + "," + std::to_string(itemId) +
                      ",'" + Db::escape(content) + "','" + Db::escape(blobOf(vec)) + "')";
    return Db::update(sql);
}

std::vector<VecHit> VectorStore::topK(const std::string& kind,
                                      const std::vector<long long>& groups,
                                      const std::vector<float>& queryVec, int k) {
    std::vector<VecHit> hits;
    if (kind != "kb" && kind != "msg") return hits;
    if (k <= 0 || queryVec.empty()) return hits;

    std::string filter = "group_id IN (";
    if (groups.empty()) {
        filter = "1=1";
    } else {
        for (size_t i = 0; i < groups.size(); ++i) {
            if (i) filter += ",";
            filter += std::to_string(groups[i]);
        }
        filter += ")";
    }

    std::string sql = "SELECT item_id, group_id, content, vec FROM vec_index WHERE kind='" +
                      kind + "' AND " + filter;
    if (!Db::query(sql)) return hits;

    struct Cands { float score; VecHit hit; };
    std::vector<Cands> cands;
    cands.reserve(256);
    while (Db::next()) {
        VecHit h;
        h.itemId = atoll(Db::value(0).c_str());
        h.groupId = atoll(Db::value(1).c_str());
        h.content = Db::value(2);
        std::vector<float> row = vecOfBlob(Db::valueBlob(3));
        if (row.size() != queryVec.size()) continue;
        double dot = 0.0;
        for (size_t i = 0; i < row.size(); ++i) dot += (double)row[i] * (double)queryVec[i];
        h.score = (float)dot;
        if (dot > 0.0) cands.push_back(Cands{(float)dot, h});
    }

    std::sort(cands.begin(), cands.end(),
              [](const Cands& a, const Cands& b) { return a.score > b.score; });
    int take = std::min<int>((int)cands.size(), k);
    for (int i = 0; i < take; ++i) hits.push_back(cands[i].hit);
    return hits;
}

bool VectorStore::clearGroup(const std::string& kind, long long groupId) {
    if (kind != "kb" && kind != "msg") return false;
    Db::update("DELETE FROM vec_index WHERE kind='" + kind + "' AND group_id=" + std::to_string(groupId));
    if (kind == "msg")
        Db::update("DELETE FROM vec_state WHERE kind='msg' AND group_id=" + std::to_string(groupId));
    return true;
}

bool VectorStore::getState(const std::string& kind, long long groupId, long long& lastItemId) {
    lastItemId = 0;
    if (Db::query("SELECT last_item_id FROM vec_state WHERE kind='" + kind +
                  "' AND group_id=" + std::to_string(groupId))) {
        if (Db::next()) lastItemId = atoll(Db::value(0).c_str());
    }
    return true;
}

void VectorStore::setState(const std::string& kind, long long groupId, long long lastItemId) {
    Db::update("INSERT INTO vec_state(kind, group_id, last_item_id) VALUES('" + kind + "'," +
               std::to_string(groupId) + "," + std::to_string(lastItemId) +
               ") ON DUPLICATE KEY UPDATE last_item_id=VALUES(last_item_id)");
}