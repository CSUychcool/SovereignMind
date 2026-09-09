#include "Router.h"
#include "ControlHandler.h"
#include "AuthHandler.h"
#include "ConvHandler.h"
#include "ChatHandler.h"
#include "KnowledgeHandler.h"
#include "VoiceHandler.h"
#include "service/AuthService.h"

void Router::dispatch(HttpContext& ctx) {
    // 公开接口 (无鉴权): /api/auth/* 整体放行, AuthHandler 内部对 logout/me 自行 401
    if (ctx.url.rfind("/api/auth", 0) == 0) {
        AuthHandler::handle(ctx);
        return;
    }

    // 需登录接口
    if (ctx.url.rfind("/api/kb", 0) == 0 ||
        ctx.url.rfind("/api/convs", 0) == 0 ||
        ctx.url.rfind("/api/control/", 0) == 0 ||
        ctx.url.rfind("/api/chat", 0) == 0 ||
        ctx.url.rfind("/api/tts", 0) == 0 ||
        ctx.url.rfind("/api/asr", 0) == 0) {
        if (!requireAuth(ctx)) return;
        if (ctx.url.rfind("/api/kb", 0) == 0)            KnowledgeHandler::handle(ctx);
        else if (ctx.url.rfind("/api/convs", 0) == 0)    ConvHandler::handle(ctx);
        else if (ctx.url.rfind("/api/control/", 0) == 0) ControlHandler::handle(ctx);
        else if (ctx.url.rfind("/api/tts", 0) == 0 ||
                 ctx.url.rfind("/api/asr", 0) == 0)      VoiceHandler::handle(ctx);
        else                                             ChatHandler::handle(ctx);
        return;
    }

    // 其余: 静态页面(公开) / 未知路由, 由 ChatHandler 兜底
    ChatHandler::handle(ctx);
}

bool Router::requireAuth(HttpContext& ctx) {
    ctx.uid = AuthService::userIdForToken(ctx.token);
    if (ctx.uid < 0) {
        ctx.resp.sendUnauthorized();
        return false;
    }
    return true;
}