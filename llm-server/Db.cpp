#include "Db.h"
#include "Log.h"
#include "config/AppConfig.h"
#include <mysql.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ---------------- 内部状态 ----------------
static MYSQL*             g_conn    = nullptr;   // MySQL 连接(单连接, 互斥保护)
static MYSQL_RES*         g_result  = nullptr;   // 最近一次 SELECT 的结果集
static MYSQL_ROW          g_row     = nullptr;   // 当前行
static std::mutex         g_mutex;               // 串行化所有 DB 操作
static std::string        g_host    = "127.0.0.1";
static unsigned short     g_port    = 3306;
static std::string        g_user    = "llm_chat";
static std::string        g_pass;
static std::string        g_db      = "llm_chat";

static void freeResult() {
    if (g_result) { mysql_free_result(g_result); g_result = nullptr; }
    g_row = nullptr;
}

static bool doConnect() {
    if (g_conn) { mysql_close(g_conn); g_conn = nullptr; }
    g_conn = mysql_init(nullptr);
    if (!g_conn) return false;
    mysql_options(g_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(g_conn, g_host.c_str(), g_user.c_str(), g_pass.c_str(),
                            g_db.c_str(), g_port, nullptr, 0)) {
        tprintf("[Db] connect failed: %s\n", mysql_error(g_conn));
        mysql_close(g_conn);
        g_conn = nullptr;
        return false;
    }
    tprintf("[Db] connected to %s:%d/%s as %s\n", g_host.c_str(), g_port, g_db.c_str(), g_user.c_str());
    fflush(stdout);
    return true;
}

// 建表 (幂等, 首次启动自动初始化)
static bool createTables() {
    static const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS users ("
            "id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "username VARCHAR(64) NOT NULL UNIQUE,"
            "email VARCHAR(128) NOT NULL UNIQUE,"
            "salt CHAR(32) NOT NULL,"
            "password_hash CHAR(64) NOT NULL,"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS sessions ("
            "token CHAR(64) PRIMARY KEY,"
            "user_id INT UNSIGNED NOT NULL,"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "expires_at DATETIME NOT NULL,"
            "INDEX(user_id), INDEX(expires_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS conversations ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "user_id INT UNSIGNED NOT NULL,"
            "title VARCHAR(120) NOT NULL DEFAULT '新对话',"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "summary TEXT,"
            "summary_upto BIGINT UNSIGNED NOT NULL DEFAULT 0,"
            "INDEX(user_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS messages ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "conv_id BIGINT UNSIGNED NOT NULL,"
            "role ENUM('user','ai') NOT NULL,"
            "content MEDIUMTEXT NOT NULL,"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "INDEX(conv_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS kb_documents ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "user_id INT UNSIGNED NOT NULL,"
            "title VARCHAR(200) NOT NULL,"
            "filename VARCHAR(255) NOT NULL,"
            "chunk_count INT NOT NULL DEFAULT 0,"
            "status ENUM('uploading','ready','failed') NOT NULL DEFAULT 'uploading',"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "INDEX(user_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS kb_chunks ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "doc_id BIGINT UNSIGNED NOT NULL,"
            "idx INT NOT NULL,"
            "content MEDIUMTEXT NOT NULL,"
            "meta_json VARCHAR(1024) DEFAULT '',"
            "INDEX(doc_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS kb_entities ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "doc_id BIGINT UNSIGNED NOT NULL,"
            "name VARCHAR(128) NOT NULL,"
            "type VARCHAR(32) DEFAULT '',"
            "INDEX(doc_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS kb_edges ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "doc_id BIGINT UNSIGNED NOT NULL,"
            "from_entity_id BIGINT UNSIGNED NOT NULL,"
            "to_entity_id BIGINT UNSIGNED NOT NULL,"
            "relation VARCHAR(64) NOT NULL,"
            "INDEX(doc_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS vec_index ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "kind ENUM('kb','msg') NOT NULL,"
            "group_id BIGINT UNSIGNED NOT NULL,"
            "item_id BIGINT UNSIGNED NOT NULL,"
            "content MEDIUMTEXT NOT NULL,"
            "vec BLOB NOT NULL,"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "INDEX(kind, group_id), INDEX(kind, item_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS vec_state ("
            "kind ENUM('kb','msg') NOT NULL,"
            "group_id BIGINT UNSIGNED NOT NULL,"
            "last_item_id BIGINT UNSIGNED NOT NULL DEFAULT 0,"
            "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY(kind, group_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    };
    for (const char* sql : ddl) {
        if (mysql_real_query(g_conn, sql, (unsigned long)strlen(sql)) != 0) {
            tprintf("[Db] create table failed: %s\n", mysql_error(g_conn));
            fflush(stdout);
            return false;
        }
    }
    return true;
}

// ---- 老库结构演进 (幂等补列) ----
static bool hasColumn(const std::string& table, const std::string& col) {
    std::string sql = "SELECT COUNT(*) FROM information_schema.columns "
                      "WHERE table_schema = DATABASE() AND table_name='" + table +
                      "' AND column_name='" + col + "'";
    if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) return false;
    MYSQL_RES* r = mysql_store_result(g_conn);
    if (!r) return false;
    MYSQL_ROW row = mysql_fetch_row(r);
    bool has = row && row[0] && atoi(row[0]) > 0;
    mysql_free_result(r);
    return has;
}

static bool ensureColumn(const std::string& table, const std::string& col, const std::string& ddl) {
    if (hasColumn(table, col)) return true;
    std::string sql = "ALTER TABLE " + table + " ADD " + ddl;
    int rc = mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size());
    if (rc != 0) tprintf("[Db] ALTER %s.%s failed: %s\n", table.c_str(), col.c_str(), mysql_error(g_conn));
    return rc == 0;
}

// ---------------- 对外接口 ----------------

bool Db::init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const AppConfig& c = AppConfig::get();
    if (!c.hasDb) {
        tprintf("[Db] no 'db' section in AppConfig, DB disabled\n");
        fflush(stdout);
        return false;
    }
    g_host = c.dbHost;
    g_port = (unsigned short)c.dbPort;
    g_user = c.dbUser;
    g_pass = c.dbPassword;
    g_db   = c.dbName;
    if (!doConnect()) return false;
    if (!createTables()) { freeResult(); mysql_close(g_conn); g_conn = nullptr; return false; }
    // P1: 老库补摘要列 (幂等)
    ensureColumn("conversations", "summary", "summary TEXT");
    ensureColumn("conversations", "summary_upto", "summary_upto BIGINT UNSIGNED NOT NULL DEFAULT 0");
    return true;
}

bool Db::isReady() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_conn != nullptr;
}

bool Db::ensureConnected() {
    if (g_conn) return true;
    return doConnect();
}

std::string Db::escape(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_conn) return s;
    char* buf = new char[s.size() * 2 + 1];
    long n = mysql_real_escape_string(g_conn, buf, s.data(), (unsigned long)s.size());
    std::string out = n >= 0 ? std::string(buf, (size_t)n) : s;
    delete[] buf;
    return out;
}

bool Db::update(const std::string& sql) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ensureConnected()) return false;
    if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) {
        tprintf("[Db] update err: %s\n    sql=%.160s\n", mysql_error(g_conn), sql.c_str());
        fflush(stdout);
        if (!doConnect()) return false;          // 断连重连后重试一次
        if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) return false;
    }
    return true;
}

long long Db::insert(const std::string& sql) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ensureConnected()) return -1;
    if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) {
        tprintf("[Db] insert err: %s\n    sql=%.160s\n", mysql_error(g_conn), sql.c_str());
        fflush(stdout);
        if (!doConnect()) return -1;
        if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) return -1;
    }
    return (long long)mysql_insert_id(g_conn);
}

bool Db::query(const std::string& sql) {
    std::lock_guard<std::mutex> lock(g_mutex);
    freeResult();
    if (!ensureConnected()) return false;
    if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) {
        tprintf("[Db] query err: %s\n    sql=%.160s\n", mysql_error(g_conn), sql.c_str());
        fflush(stdout);
        if (!doConnect()) return false;
        if (mysql_real_query(g_conn, sql.data(), (unsigned long)sql.size()) != 0) return false;
    }
    g_result = mysql_store_result(g_conn);
    return g_result != nullptr;
}

bool Db::next() {
    if (!g_result) return false;
    g_row = mysql_fetch_row(g_result);
    return g_row != nullptr;
}

std::string Db::value(int index) {
    if (!g_result || !g_row) return "";
    unsigned int n = mysql_num_fields(g_result);
    if (index < 0 || (unsigned)index >= n) return "";
    return g_row[index] ? g_row[index] : "";
}

std::string Db::valueBlob(int index) {
    if (!g_result || !g_row) return "";
    unsigned int n = mysql_num_fields(g_result);
    if (index < 0 || (unsigned)index >= n) return "";
    if (!g_row[index]) return "";
    unsigned long* lens = mysql_fetch_lengths(g_result);   // 列表长度里才有 BLOB 真实字节数
    if (!lens) return "";
    return std::string(g_row[index], (size_t)lens[index]);
}

void Db::close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    freeResult();
    if (g_conn) { mysql_close(g_conn); g_conn = nullptr; }
}