#include "EmbedClient.h"
#include "config/AppConfig.h"
#include "Log.h"
#include <json/json.h>
#include <json/reader.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <algorithm>

// 执行 transfer-encoding: chunked 反帧
std::string EmbedClient::decodeChunked(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t nl = s.find("\r\n", i);
        if (nl == std::string::npos) break;
        std::string sizeStr = s.substr(i, nl - i);
        size_t semi = sizeStr.find(';');
        if (semi != std::string::npos) sizeStr = sizeStr.substr(0, semi);
        long n = strtol(sizeStr.c_str(), nullptr, 16);
        if (n <= 0) break;                       // 0 长度终止块
        i = nl + 2;
        if (i + (size_t)n > s.size()) break;
        out.append(s, i, (size_t)n);
        i += (size_t)n;
        if (i + 1 < s.size() && s[i] == '\r' && s[i + 1] == '\n') i += 2;
        else if (i < s.size() && s[i] == '\n') i += 1;
    }
    return out;
}

void EmbedClient::normalize(std::vector<float>& v) {
    double accum = 0.0;
    for (float x : v) accum += (double)x * (double)x;
    double norm = std::sqrt(accum);
    if (norm > 1e-9) {
        for (float& x : v) x = (float)((double)x / norm);
    } else {
        std::fill(v.begin(), v.end(), 0.f);
    }
}

bool EmbedClient::embed(const std::string& text, bool asQuery, std::vector<float>& out) {
    std::vector<std::string> texts{text};
    std::vector<std::vector<float>> outs;
    if (!embedBatch(texts, asQuery, outs) || outs.empty()) return false;
    out = std::move(outs[0]);
    return true;
}

bool EmbedClient::embedBatch(const std::vector<std::string>& texts, bool asQuery,
                             std::vector<std::vector<float>>& out) {
    const AppConfig& c = AppConfig::get();
    if (texts.empty()) return true;

    Json::Value input(Json::arrayValue);
    for (const auto& t : texts) {
        std::string s = t;
        if (c.embedTaskPrefix) s = (asQuery ? std::string("search_query: ") : std::string("search_document: ")) + s;
        input.append(s);
    }
    Json::Value req;
    req["model"] = c.embedModel;
    req["input"] = input;
    req["truncate"] = true;
    std::string body = Json::FastWriter().write(req);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons((unsigned short)c.embedPort);
    if (inet_pton(AF_INET, c.embedHost.c_str(), &srv.sin_addr) != 1) { ::close(sock); return false; }
    if (::connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) { ::close(sock); return false; }

    std::string reqData =
        "POST /api/embed HTTP/1.1\r\n"
        "Host: " + c.embedHost + ":" + std::to_string(c.embedPort) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    if (::send(sock, reqData.data(), reqData.size(), MSG_NOSIGNAL) < 0) { ::close(sock); return false; }

    std::string resp;
    char buf[8192];
    int n;
    while ((n = (int)::recv(sock, buf, sizeof(buf), 0)) > 0) resp.append(buf, (size_t)n);
    ::close(sock);

    size_t he = resp.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    std::string head = resp.substr(0, he);
    std::string payload = resp.substr(he + 4);
    if (head.find("chunked") != std::string::npos) payload = decodeChunked(payload);

    Json::Value root;
    Json::Reader rd;
    if (!rd.parse(payload, root)) {
        tprintf("[EmbedClient] parse FAILED: %.160s\n", payload.c_str());
        return false;
    }

    const Json::Value& arr = root["embeddings"];
    if (arr.isArray() && arr.size() > 0) {
        for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
            if (!arr[i].isArray()) continue;
            std::vector<float> v;
            v.reserve(arr[i].size());
            for (Json::ArrayIndex j = 0; j < arr[i].size(); ++j) v.push_back((float)arr[i][j].asDouble());
            normalize(v);
            out.push_back(std::move(v));
        }
    } else {
        const Json::Value& emb = root["embedding"];
        if (!emb.isArray()) return false;
        std::vector<float> v;
        v.reserve(emb.size());
        for (Json::ArrayIndex j = 0; j < emb.size(); ++j) v.push_back((float)emb[j].asDouble());
        normalize(v);
        out.push_back(std::move(v));
    }
    return !out.empty();
}