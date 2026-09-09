#pragma once

#include <string>

// MySQL 封装: 自包含内存的 C API 使用, 供 llm-server 的鉴权/会话/消息落库使用。
// - 单连接 + 互斥量串行化 (llm-server 4 worker 线程, 个人场景足够)
// - 断连后自动重连一次
// - SQL 值必须经 escape() 转义防注入
class Db {
public:
    // 从 AppConfig 读取 db 配置并连接 + 自动建表(幂等)。要求先调用 AppConfig::load()
    static bool init();
    static bool isReady();

    // 对字符串做 SQL 转义 (返回可安全放进单引号字面量的内容)
    static std::string escape(const std::string& s);

    // INSERT/UPDATE/DELETE; 失败返回 false
    static bool update(const std::string& sql);
    // INSERT 且返回自增 id (同一把锁内执行+取id, 不会串号); 失败返回 -1
    static long long insert(const std::string& sql);

    // SELECT: query() 成功后用 next() 逐行遍历, value(i) 取列(从0开始)
    static bool query(const std::string& sql);
    static bool next();
    static std::string value(int index);
    // 取列原始字节(含 '\0', 二进制安全, 供 BLOB 向量使用)
    static std::string valueBlob(int index);

    static void close();

private:
    static bool ensureConnected();
};