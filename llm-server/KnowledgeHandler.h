#pragma once

struct HttpContext;

// /api/kb/* HTTP 层: 知识库上传/列表/删除 (需登录)
// 逻辑走 service/KnowledgeService; 不碰 SQL / 不碰 fd
class KnowledgeHandler {
public:
    static void handle(HttpContext& ctx);
};