#include "Log.h"
#include "ChatHandler.h"
#include "net/HttpContext.h"
#include "service/ConversationService.h"
#include "service/LlmGateway.h"
#include "service/TokenCounter.h"
#include "config/AppConfig.h"
#include <json/json.h>
#include <json/reader.h>
#include <cstdlib>
#include <cstring>
#include <vector>

// P1 滚动压缩: 把最早一批旧消息丢给模型生成摘要, 合并覆盖 conversations.summary 并裁剪原消息。
// 压缩模式(AppConfig::compactionMode):
//   - "until_fit"(默认): 单请求内循环连压, 直到未压缩活跃段可装进预算(hasOlder=false)或达 maxCompressRounds
//   - "throttled"      : 旧节流, 新增>=summaryMinNewMessages 才触发, 每请求最多一轮
static const int kCompressChunk = 40;
static void tryCompaction(long long convId, long long lastMsgId, int budget) {
    const AppConfig& cfg = AppConfig::get();
    const bool untilFit = (cfg.compactionMode == "until_fit");

    // 用与加载历史相同的预算口径重算"是否仍溢出"
    auto stillOverflow = [&]() {
        std::vector<MsgRow> probe;
        bool hasOlder = false;
        ConversationService::loadHistoryByTokens(convId, 0, budget,
                                                 cfg.historyFetchLimit, probe, hasOlder);
        return hasOlder;
    };

    for (int round = 0; round < cfg.maxCompressRounds; ++round) {
        long long upto = 0;
        std::string existing;
        ConversationService::getSummaryEx(convId, existing, upto);

        // throttled 模式: 距上次摘要新增不足 -> 不压
        if (!untilFit && ConversationService::messagesSince(convId, upto) < cfg.summaryMinNewMessages) {
            return;
        }

        // 取材: 全局最早的一批原文 (until_fit 下 summary_upto 始终=最新, 故直接取全局最早)
        std::vector<MsgRow> oldest;
        long long maxId = 0;
        ConversationService::loadOldestMessages(convId, kCompressChunk, oldest, maxId);
        if (oldest.empty()) return;

        // 素材: 已有摘要 + 最早的一批旧消息
        std::string ending = oldest[0].role + ": " + oldest[0].content;
        for (size_t i = 1; i < oldest.size(); ++i)
            ending += "\n" + oldest[i].role + ": " + oldest[i].content;

        Json::Value req;
        req["model"] = cfg.model;
        req["stream"] = true;
        Json::Value messages(Json::arrayValue);
        Json::Value sys;
        sys["role"] = "system";
        sys["content"] = "你是对话历史归档器。综合【已有摘要】和【新增早期消息】，输出更新后的完整摘要（不超过800字，中文，"
                         "保留关键任务/决定/代码主题；无新增内容时可基本保持原文）。只输出摘要文本。";
        messages.append(sys);
        Json::Value usr;
        usr["role"] = "user";
        usr["content"] = "【已有摘要】\n" + (existing.empty() ? std::string("(无)") : existing) +
                         "\n\n【新增早期消息】\n" + ending;
        messages.append(usr);
        req["messages"] = messages;

        std::string merged;
        if (!LlmGateway::summarize(req, merged) || merged.empty()) return;   // 失败不循环, 防死循环
        if (merged.size() > 2000) merged = merged.substr(0, 2000);            // 摘要配额上限
        ConversationService::setSummary(convId, merged, lastMsgId);
        ConversationService::trimOldestMessages(convId, (int)oldest.size());
        tprintf("[ChatHandler] compaction[%s] round %d/%d: %zu oldest msgs -> summary (%zu chars)\n",
                untilFit ? "until_fit" : "throttled", round + 1, cfg.maxCompressRounds,
                oldest.size(), merged.size());
        fflush(stdout);

        if (!untilFit) return;                 // throttled: 每请求最多一轮
        if (!stillOverflow()) break;           // until_fit: 预算已装下 -> 收敛; 否则继续连压
    }
}

// P3: 窗口外的更旧历史, 用提问关键词做粗召回并入上下文 (占用剩余预算, 最多 6 条)
// 中文不做分词, 采用"按 ASCII 空白/标点切词取最长词; 无词可切时取句中部 16 字符"的近似
static std::string extractKeyword(const std::string& s) {
    std::string best, cur;
    auto isSep = [](unsigned char ch) {
        if (ch <= 32) return true;
        return strchr(".,;:!?()[]{}\"'`<>/\\|-=_+*&^%$#@~", (char)ch) != nullptr;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (isSep(ch)) { if (cur.size() > best.size()) best = cur; cur.clear(); }
        else cur.push_back((char)ch);
    }
    if (cur.size() > best.size()) best = cur;
    if (best.size() < 2 && s.size() >= 8) {           // 中文长句无空格: 取中间一段
        size_t mid = s.size() / 2;
        best = s.substr(mid - 6, 12);
    }
    return best.size() >= 2 ? best : "";
}

static void joinKeyboardRecall(long long convId, const std::string& prompt,
                               int& budget, Json::Value& messages) {
    std::string kw = extractKeyword(prompt);
    if (kw.empty()) return;
    std::vector<MsgRow> hits;
    ConversationService::searchMessages(convId, kw, 6, hits);
    int added = 0;
    for (const auto& h : hits) {
        int tok = TokenCounter::estimateTokens(h.content) + 4;
        if (budget - tok < 500) break;                // 至少留 500 token 余量给提问
        budget -= tok;
        Json::Value m;
        m["role"] = h.role;
        m["content"] = h.content;
        messages.append(m);
        added++;
    }
    if (added > 0) {
        tprintf("[ChatHandler] P3 recall: +%d older msgs for kw='%.20s'\n", added, kw.c_str());
        fflush(stdout);
    }
}

void ChatHandler::handle(HttpContext& ctx) {
    // ---- 静态页面: 公开 ----
    if (ctx.method == "GET" &&
        ctx.resp.serveWebRootFile(AppConfig::get().webRoot, ctx.url)) {
        return;   // 已命中并由 Response 发送
    }

    // ---- 仅接受 POST /api/chat ----
    if (ctx.url.rfind("/api/chat", 0) != 0) { ctx.resp.sendErr("unknown route"); return; }

    // 鉴权已由 route/Router 中间件完成: ctx.uid 即当前用户

    Json::Value req;
    Json::Reader reader;
    if (!reader.parse(std::string(ctx.body ? ctx.body : "", ctx.bodyLen), req)) {
        ctx.resp.sendErr("参数格式错误"); return;
    }
    std::string prompt = req["prompt"].asString();
    std::string systemPrompt = req.get("system_prompt", "You are a helpful AI assistant.").asString();

    // ---- 持久化 user 消息 + 归属校验 ----
    long long convId = atoll(req.get("conv_id", "0").asString().c_str());
    bool persist = convId > 0;
    std::string userMsgId;   // 本次 user 消息行 id, 用于取"它之前"的历史
    long long uid = ctx.uid; // Router 中间件已鉴权
    if (persist) {
        if (!ConversationService::ownsConversation(uid, convId)) {
            ctx.resp.sendErr("对话不存在或无权访问"); return;
        }
        long long mid = ConversationService::appendMessage(convId, "user", prompt);
        if (mid > 0) userMsgId = std::to_string(mid);
    }

    // ---- 预算: 窗口 - 输出预留 - 系统提示 - 本次提问 - 摘要配额 ----
    int budget = AppConfig::get().usableHistoryTokens()
                 - TokenCounter::estimateTokens(systemPrompt)
                 - TokenCounter::estimateTokens(prompt);

    // ---- 组装 OpenAI messages (system + [摘要] + history + user) ----
    Json::Value messages(Json::arrayValue);
    if (!systemPrompt.empty()) {
        Json::Value sysMsg;
        sysMsg["role"] = "system";
        sysMsg["content"] = systemPrompt;
        messages.append(sysMsg);
    }

    // P1: 滚动摘要并入上下文 (占预算, 在 history 之前, 让模型先"记住"旧事)
    std::string summaryText;
    if (persist && ConversationService::getSummary(convId, summaryText) && !summaryText.empty()) {
        int sumTok = TokenCounter::estimateTokens(summaryText) + 8;
        if (sumTok < budget) {
            budget -= sumTok;
            Json::Value sm;
            sm["role"] = "user";
            sm["content"] = "【此前对话要点】\n" + summaryText;
            messages.append(sm);
        }
    }

    // ---- 历史: 前端显式传则用之, 否则按 token 预算从最新往最旧装 ----
    bool hasOlder = false;
    Json::Value hist = req.get("history", Json::arrayValue);
    if (hist.isArray() && hist.size() > 0) {
        for (Json::ArrayIndex i = 0; i < hist.size(); ++i) messages.append(hist[i]);
    } else if (persist) {
        std::vector<MsgRow> hs;
        ConversationService::loadHistoryByTokens(convId,
            userMsgId.empty() ? 0 : atoll(userMsgId.c_str()),
            budget, AppConfig::get().historyFetchLimit, hs, hasOlder);
        for (const auto& h : hs) {
            Json::Value m;
            m["role"] = h.role;
            m["content"] = h.content;
            messages.append(m);
        }
        tprintf("[ChatHandler] db history loaded: %zu msgs (budget %d tok, older=%d)\n",
                hs.size(), budget, hasOlder ? 1 : 0);
        fflush(stdout);
        // P3: 预算仍有富余且窗口外还有更旧历史时, 用提问关键词召回
        if (hasOlder && budget > 1000) joinKeyboardRecall(convId, prompt, budget, messages);
    }

    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = prompt;
    messages.append(userMsg);

    // ---- 调用上游网关 (SSE 流式回传 + 累积 AI 回复) ----
    Json::Value openaiReq;
    openaiReq["model"] = AppConfig::get().model;
    openaiReq["messages"] = messages;
    openaiReq["stream"] = true;
    openaiReq["temperature"] = 0.7;

    std::string aiFull;
    LlmGateway::chatStream(openaiReq, ctx.resp, aiFull);

    // ---- AI 回复落库 + P1 滚动压缩触发 ----
    if (persist && !aiFull.empty()) {
        long long aiMsgId = ConversationService::appendMessage(convId, "ai", aiFull);
        tprintf("[ChatHandler] persisted AI reply (%zu bytes)\n", aiFull.size());
        fflush(stdout);
        if (hasOlder && aiMsgId > 0) tryCompaction(convId, aiMsgId, budget);
    }
    // Response 已由网关在退出时关闭, 无需再处理
}