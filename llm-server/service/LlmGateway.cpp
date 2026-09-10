#include "LlmGateway.h"
#include "Log.h"
#include "config/AppConfig.h"
#include "http_common/Response.h"
#include <json/value.h>
#include <json/reader.h>
#include <json/writer.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctype.h>

// 去除上游 HTTP chunked 编码残留的帧头(hex 长度行)与 CRLF
static void stripChunkedFraming(std::string& s) {
    while (true) {
        if (s.rfind("\r\n", 0) == 0) { s.erase(0, 2); continue; }
        if (!s.empty() && s[0] == '\n') { s.erase(0, 1); continue; }
        size_t nl = s.find("\r\n");
        if (nl != std::string::npos && nl > 0) {
            bool allHex = true;
            for (size_t i = 0; i < nl; ++i)
                if (!isxdigit((unsigned char)s[i])) { allHex = false; break; }
            if (allHex) { s.erase(0, nl + 2); continue; }
        }
        break;
    }
}

bool LlmGateway::chatStream(const Json::Value& openaiReq, Response& resp, std::string& aiFull,
                            const Json::Value* contextEvent) {
    return relay(openaiReq, &resp, aiFull, contextEvent);
}

bool LlmGateway::summarize(const Json::Value& openaiReq, std::string& outText) {
    return relay(openaiReq, nullptr, outText);
}

bool LlmGateway::relay(const Json::Value& openaiReq, Response* resp, std::string& out,
                       const Json::Value* ctxEvent) {
    const AppConfig& c = AppConfig::get();
    std::string requestBody = Json::FastWriter().write(openaiReq);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (resp) resp->sendErr("internal: socket() failed");
        return false;
    }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons((unsigned short)c.upstreamPort);
    if (inet_pton(AF_INET, c.upstreamHost.c_str(), &srv.sin_addr) != 1) {
        tprintf("[LlmGateway] cannot resolve upstream %s\n", c.upstreamHost.c_str());
        if (resp) resp->sendErr("上游地址无法解析");
        ::close(sock);
        return false;
    }
    if (::connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("[LlmGateway] connect");
        if (resp) resp->sendErr("无法连接上游模型服务");
        ::close(sock);
        return false;
    }

    std::string reqLine = "POST " + c.upstreamPath + " HTTP/1.1\r\n";
    reqLine += "Host: " + c.upstreamHost + ":" + std::to_string(c.upstreamPort) + "\r\n";
    reqLine += "Content-Type: application/json\r\n";
    reqLine += "Content-Length: " + std::to_string(requestBody.size()) + "\r\n";
    reqLine += "Connection: close\r\n\r\n";
    ::send(sock, reqLine.data(), reqLine.size(), MSG_NOSIGNAL);
    ::send(sock, requestBody.data(), requestBody.size(), MSG_NOSIGNAL);

    if (resp) resp->beginStream();

    char readBuf[8192];
    std::string accum;
    bool done = false;
    bool headersSkipped = false;
    bool firstChunk = true;
    int chunkCount = 0;
    while (!done) {
        int n = recv(sock, readBuf, sizeof(readBuf), 0);
        if (n <= 0) break;
        accum.append(readBuf, (size_t)n);

        if (!headersSkipped) {
            size_t he = accum.find("\r\n\r\n");
            if (he != std::string::npos) { accum.erase(0, he + 4); headersSkipped = true; }
        }

        size_t pos;
        while (!done && (pos = accum.find("\n\n")) != std::string::npos) {
            std::string block = accum.substr(0, pos + 2);
            accum.erase(0, pos + 2);
            stripChunkedFraming(block);

            size_t dataPos;
            while ((dataPos = block.find("data: ")) != std::string::npos) {
                block.erase(dataPos, 6);
                size_t endLine = block.find('\n');
                std::string dataChunk = (endLine != std::string::npos) ? block.substr(0, endLine) : block;
                block.erase(0, endLine != std::string::npos ? endLine + 1 : block.size());

                if (dataChunk.find("\"[DONE]") != std::string::npos || dataChunk == "[DONE]") {
                    if (ctxEvent && resp) resp->sseChunk(Json::FastWriter().write(*ctxEvent));   // 上下文透明事件
                    if (resp) resp->sseChunk("[DONE]");
                    done = true;
                    break;
                }

                Json::Value chunkObj;
                Json::Reader chunkReader;
                if (chunkReader.parse(dataChunk, chunkObj)) {
                    std::string content;
                    Json::Value& choices = chunkObj["choices"];
                    if (choices.isArray() && choices.size() > 0) {
                        Json::Value& delta = choices[0]["delta"];
                        content = delta.get("content", "").asString();
                    }
                    if (content.empty()) {
                        // 兼容非流式 message.content
                        content = chunkObj["choices"][0]["message"].get("content", "").asString();
                    }
                    if (!content.empty()) {
                        if (firstChunk) {
                            tprintf("[LlmGateway] FIRST token: \"%.40s...\"\n", content.c_str());
                            firstChunk = false;
                        }
                        chunkCount++;
                        out += content;
                        if (resp) resp->sseChunk(content);
                    }
                } else {
                    tprintf("[LlmGateway] parse FAILED, raw=[%.160s]\n", dataChunk.c_str());
                }
            }
        }
    }
    ::close(sock);
    if (resp) resp->finish();
    tprintf("[LlmGateway] COMPLETED, %d chunks, %zu bytes\n", chunkCount, out.size());
    fflush(stdout);
    return true;
}