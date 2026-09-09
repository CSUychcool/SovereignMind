#include "KnowledgeHandler.h"
#include "net/HttpContext.h"
#include "service/KnowledgeService.h"
#include <json/json.h>
#include <json/reader.h>
#include <cstdlib>

using namespace std;

// 知识库 HTTP 层: 鉴权已由 Router 完成, ctx.uid 即当前用户
void KnowledgeHandler::handle(HttpContext& ctx) {
    long long uid = ctx.uid;

    // ---- 上传 (POST /api/kb/upload) {title?, filename, content} ----
    if (ctx.method == "POST" && ctx.url == "/api/kb/upload") {
        Json::Value req;
        Json::Reader reader;
        if (!reader.parse(string(ctx.body ? ctx.body : "", ctx.bodyLen), req)) {
            ctx.resp.sendErr("参数格式错误"); return;
        }
        string title = req.get("title", "未命名知识库").asString();
        string filename = req.get("filename", "doc.txt").asString();
        string content = req.get("content", "").asString();
        if (!KnowledgeService::uploadDocument(uid, title, filename, content)) {
            ctx.resp.sendErr("上传失败(当前支持 txt/md, 或内容为空)"); return;
        }
        Json::Value resp;
        resp["code"] = 0;
        ctx.resp.sendJson(200, "OK", resp);
        return;
    }

    // ---- 列表 (GET /api/kb/list) ----
    if (ctx.method == "GET" && ctx.url == "/api/kb/list") {
        vector<KbDocRow> docs;
        KnowledgeService::listDocuments(uid, docs);
        Json::Value resp;
        resp["code"] = 0;
        Json::Value arr(Json::arrayValue);
        for (const auto& d : docs) {
            Json::Value o;
            o["id"] = to_string(d.id);
            o["title"] = d.title;
            o["filename"] = d.filename;
            o["chunk_count"] = d.chunkCount;
            o["status"] = d.status;
            o["created_at"] = d.createdAt;
            arr.append(o);
        }
        resp["docs"] = arr;
        ctx.resp.sendJson(200, "OK", resp);
        return;
    }

    // ---- 删除 (POST /api/kb/delete) {id} ----
    if (ctx.method == "POST" && ctx.url == "/api/kb/delete") {
        Json::Value req;
        Json::Reader reader;
        if (!reader.parse(string(ctx.body ? ctx.body : "", ctx.bodyLen), req)) {
            ctx.resp.sendErr("参数格式错误"); return;
        }
        long long id = atoll(req.get("id", "0").asString().c_str());
        if (id <= 0) { ctx.resp.sendErr("参数格式错误"); return; }
        if (!KnowledgeService::removeDocument(uid, id)) {
            ctx.resp.sendErr("文档不存在或无权访问"); return;
        }
        Json::Value resp;
        resp["code"] = 0;
        ctx.resp.sendJson(200, "OK", resp);
        return;
    }

    ctx.resp.sendErr("unknown kb route");
}