#include "Response.h"
#include "Log.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

Response::Response(int fd) : m_fd(fd), m_finished(false) {}

Response::~Response() { closeFd(); }

void Response::closeFd() {
    if (!m_finished && m_fd >= 0) { ::close(m_fd); }
    m_finished = true;
}

void Response::sendAll(const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = ::send(m_fd, data + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

void Response::sendJson(int code, const char* reason, const Json::Value& json) {
    char head[256];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, reason);
    std::string body = Json::FastWriter().write(json);
    sendAll(head, (size_t)hl);
    sendAll(body.data(), body.size());
    closeFd();
}

void Response::sendErr(const char* msg) {
    Json::Value j;
    j["code"] = 1;
    j["msg"] = msg;
    sendJson(200, "OK", j);
}

void Response::sendUnauthorized() {
    Json::Value j;
    j["code"] = 401;
    j["msg"] = "未登录或登录已过期";
    sendJson(401, "Unauthorized", j);
}

bool Response::sendFile(const std::string& path, const std::string& contentType) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string body = ss.str();

    char head[256];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        contentType.c_str(), body.size());
    sendAll(head, (size_t)hl);
    sendAll(body.data(), body.size());
    closeFd();
    tprintf("[Response] served static %s (%zu bytes)\n", path.c_str(), body.size());
    fflush(stdout);
    return true;
}

bool Response::serveWebRootFile(const std::string& webRoot, const std::string& url) {
    if (webRoot.empty()) return false;
    std::string u = url.empty() ? "/" : url;
    size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    if (u.find("..") != std::string::npos) return false;      // 路径穿越防御
    if (u.empty() || u == "/") u = "/index.html";
    if (u[0] != '/') return false;

    const char* ctype = "text/html; charset=utf-8";
    if (u.rfind(".css") != std::string::npos)      ctype = "text/css; charset=utf-8";
    else if (u.rfind(".js")  != std::string::npos) ctype = "text/javascript; charset=utf-8";
    else if (u.rfind(".png") != std::string::npos) ctype = "image/png";
    else if (u.rfind(".jpg") != std::string::npos || u.rfind(".jpeg") != std::string::npos)
        ctype = "image/jpeg";
    else if (u.rfind(".svg") != std::string::npos) ctype = "image/svg+xml";

    return sendFile(webRoot + u, ctype);   // 文件不存在时 sendFile 返回 false 且不关闭
}

void Response::beginStream() {
    static const char* head =
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n";
    sendAll(head, strlen(head));
}

void Response::sseChunk(const std::string& chunk) {
    // SSE 规范: 内容含换行时必须拆成多个 data: 行, 否则客户端按事件切分会丢行
    std::string payload;
    size_t start = 0;
    while (true) {
        size_t nl = chunk.find('\n', start);
        std::string line = (nl == std::string::npos) ? chunk.substr(start)
                                                      : chunk.substr(start, nl - start);
        payload += "data: " + line + "\n";
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    payload += "\n";   // 空行结束事件
    sendAll(payload.data(), payload.size());
}

void Response::finish() { closeFd(); }