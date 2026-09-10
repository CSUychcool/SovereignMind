#include "VoiceService.h"
#include "Response.h"
#include "Log.h"
#include "config/AppConfig.h"
#include <json/json.h>
#include <json/reader.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

using namespace std;

// ---------- base64 ----------
string VoiceService::base64Encode(const string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned int n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        out.push_back(T[(n >> 18) & 63]); out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);  out.push_back(T[n & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned int n = (unsigned char)in[i] << 16;
        out.push_back(T[(n >> 18) & 63]); out.push_back(T[(n >> 12) & 63]); out.push_back('='); out.push_back('=');
    } else if (i + 2 == in.size()) {
        unsigned int n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
        out.push_back(T[(n >> 18) & 63]); out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);  out.push_back('=');
    }
    return out;
}

// ---------- 分句 (中文/英文 句末标点) ----------
static bool isTerm(const string& s, size_t i, size_t& len) {
    unsigned char c = (unsigned char)s[i];
    if (c == '.' || c == '!' || c == '?' || c == '\n' || c == ';') { len = 1; return true; }
    static const char* seq[] = { "\xE3\x80\x82",   // 。
                                "\xEF\xBC\x9F",   // ？
                                "\xEF\xBC\x81",   // ！
                                "\xE2\x80\xA6" }; // …
    for (auto* q : seq) {
        size_t l = strlen(q);
        if (i + l <= s.size() && memcmp(s.data() + i, q, l) == 0) { len = l; return true; }
    }
    return false;
}

vector<string> VoiceService::splitSentences(const string& text) {
    const size_t kMax = 100;   // 单句上限(防止 piper 超长输入)
    vector<string> out;
    string buf;
    for (size_t i = 0; i < text.size();) {
        size_t tl = 0;
        if (isTerm(text, i, tl)) {
            buf.append(text, i, tl);
            if (buf.size() >= 6) {   // 短句攒一起; 首尾空白在末尾统一清理
                out.push_back(buf);
                buf.clear();
            }
            i += tl;
            continue;
        }
        buf.push_back(text[i]);
        if (buf.size() >= kMax) {    // 超长硬切
            out.push_back(buf);
            buf.clear();
        }
        ++i;
    }
    if (!buf.empty()) out.push_back(buf);
    // 去掉首尾空白与空句
    vector<string> clean;
    for (auto& s : out) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t' || (unsigned char)s[a] < 32)) ++a;
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\n' || s[b-1] == '\t' || (unsigned char)s[b-1] < 32)) --b;
        string t = s.substr(a, b - a);
        if (!t.empty()) clean.push_back(t);
    }
    return clean;
}

// ---------- piper ----------
string VoiceService::runPiper(const string& sentence, double lengthScale) {
    const AppConfig& c = AppConfig::get();
    if (sentence.empty()) return "";
    static int seq = 0;
    string base = "/tmp/llm_tts_" + to_string(::getpid()) + "_" + to_string(++seq);

    FILE* f = fopen((base + ".in").c_str(), "wb");
    if (!f) return "";
    fwrite(sentence.data(), 1, sentence.size(), f);
    fclose(f);

    string cmd = "\"" + c.piperPath + "\" --model \"" + c.piperVoice + "\"" +
                 (c.piperVoiceCfg.empty() ? "" : " --config \"" + c.piperVoiceCfg + "\"") +
                 " --length-scale " + to_string(lengthScale) +
                 " < \"" + base + ".in\" > \"" + base + ".wav\" 2>/dev/null";
    int rc = system(cmd.c_str());

    string wav;
    FILE* w = fopen((base + ".wav").c_str(), "rb");
    if (rc == 0 && w) {
        struct stat st;
        if (fstat(fileno(w), &st) == 0 && st.st_size > 44) {   // 完整 WAV 头 + 至少一帧
            wav.resize((size_t)st.st_size);
            if (fread(&wav[0], 1, wav.size(), w) != wav.size()) wav.clear();
        }
        fclose(w);
    }
    unlink((base + ".in").c_str());
    unlink((base + ".wav").c_str());
    return wav;
}

void VoiceService::streamTts(const string& text, double lengthScale, const string& voice, Response& resp) {
    resp.beginStream();
    tprintf("[Voice] TTS whole-text %zu chars (backend=%s voice=%s)\n",
            text.size(), AppConfig::get().ttsBackend.c_str(), voice.c_str());
    fflush(stdout);
    // 整段一口合成, sidecar 内部切句一次推理, 避免"句-句"间隔
    string wav = synthSentence(text, lengthScale, voice);
    if (!wav.empty()) {
        Json::Value ev;
        ev["s"] = 0;
        ev["b64"] = base64Encode(wav);
        resp.sseChunk(Json::FastWriter().write(ev));
    } else {
        tprintf("[Voice] tts fail on whole text\n");
        fflush(stdout);
    }
    resp.sseChunk("[DONE]");
    resp.finish();
}

// CosyVoice2 sidecar 优先, piper 兜底
string VoiceService::synthSentence(const string& sentence, double lengthScale, const string& voice) {
    const AppConfig& c = AppConfig::get();
    if (c.ttsBackend == "cosyvoice") {
        string w = runCosyVoice(sentence, voice);
        if (!w.empty()) return w;
        tprintf("[Voice] cosyvoice unavailable, fallback piper\n");
        fflush(stdout);
    }
    return runPiper(sentence, lengthScale);
}

string VoiceService::runCosyVoice(const string& sentence, const string& voice) {
    const AppConfig& c = AppConfig::get();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons((unsigned short)c.cosyPort);
    if (inet_pton(AF_INET, c.cosyHost.c_str(), &srv.sin_addr) != 1) { ::close(sock); return ""; }
    if (::connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) { ::close(sock); return ""; }

    Json::Value req;
    req["text"] = sentence;
    if (!voice.empty()) req["voice"] = voice;
    string body = Json::FastWriter().write(req);
    string reqData =
        "POST /tts HTTP/1.1\r\n"
        "Host: " + c.cosyHost + ":" + to_string(c.cosyPort) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    ::send(sock, reqData.data(), reqData.size(), MSG_NOSIGNAL);

    string resp;
    char buf[8192];
    int n;
    while ((n = (int)::recv(sock, buf, sizeof(buf), 0)) > 0) resp.append(buf, (size_t)n);
    ::close(sock);

    size_t he = resp.find("\r\n\r\n");
    if (he == string::npos) return "";
    string head = resp.substr(0, he);
    string payload = resp.substr(he + 4);
    if (head.find("chunked") != string::npos) {
        string out;
        size_t i = 0;
        while (i < payload.size()) {
            size_t nl = payload.find("\r\n", i);
            if (nl == string::npos) break;
            long len = strtol(payload.substr(i, nl - i).c_str(), nullptr, 16);
            if (len <= 0) break;
            i = nl + 2;
            if (i + (size_t)len > payload.size()) break;
            out.append(payload, i, (size_t)len);
            i += (size_t)len;
            if (i + 1 < payload.size() && payload[i] == '\r' && payload[i+1] == '\n') i += 2;
        }
        payload = out;
    }
    if (head.find(" 200 ") == string::npos) return "";
    return payload;
}

// ---------- whisper ----------
string VoiceService::whisperBin() {
    const AppConfig& c = AppConfig::get();
    if (access(c.whisperPath.c_str(), X_OK) == 0) return c.whisperPath;
    string alt = "/home/yc_21/server_ddz/voice/whisper.cpp/build/main";
    return alt;
}

bool VoiceService::transcribe(const string& wavData, string& text) {
    if (wavData.empty()) return false;
    static int seq = 0;
    string base = "/tmp/llm_asr_" + to_string(::getpid()) + "_" + to_string(++seq);
    FILE* f = fopen((base + ".wav").c_str(), "wb");
    if (!f) return false;
    fwrite(wavData.data(), 1, wavData.size(), f);
    fclose(f);

    const AppConfig& c = AppConfig::get();
    string cmd = "\"" + whisperBin() + "\" -m \"" + c.whisperModel + "\" -f \"" +
                 base + ".wav\" -otxt -np -nt -l " + c.whisperLang + " 2>/dev/null";
    int rc = system(cmd.c_str());

    bool ok = false;
    FILE* r = fopen((base + ".wav.txt").c_str(), "rb");
    if (rc == 0 && r) {
        char buf[8192] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, r);
        fclose(r);
        text.assign(buf, n);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r')) text.pop_back();
        while (!text.empty() && (text.front() == '\n' || text.front() == ' ')) text.erase(0, 1);
        ok = !text.empty();
    }
    unlink((base + ".wav").c_str());
    unlink((base + ".wav.txt").c_str());
    return ok;
}