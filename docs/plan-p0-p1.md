# P0 / P1 具体实施计划

- 关联文档：`docs/requirements.md` v0.6（需求基线 & 选型结论 §22 & 总体计划 §23）
- 日期：2026-09-09
- 分支：main
- 说明：本文档给出 **P0 与 P1 的任务级计划**（每个任务=一次可开工实现的单位）。进一步到"某个具体任务的实施步骤"仍按约定在真正运行时再拆。

## 0. 范围界定

| P 级 | 需求 | 交付物 | 依赖选型 |
|---|---|---|---|
| P0 | 需求一 压缩节流B（连压至 hasOlder=false） | 独立快件 | — |
| P1 | 需求二 文档上传 → RAG + RAG GRAPH | 主工程 | D1 embed(Ollama nomic) / D2 faiss / D3 GRAPH |
| P1 | 需求三 语音 ASR + TTS | 独立并行 | D4 浏览器API → 本地化 |
| P1 | 需求十八 历史会话召回向量化（路径B） | 随需求二交付 | 复用 D1/D2 基建 |

**两个开工即做的前置 spike**（1–2 人日，先于 P1 主体）：
1. faiss 在 WSL 的构建/集成方式（apt `libfaiss-dev` vs 源码编译）确认，出最小 demo（建 Index、落盘、检索）。
2. Ollama `/api/embed` 用 nomic-embed-text 验证：维度、latency、中文效果、与 faiss 内积检索的端到端联通。

---

## 1. P0：需求一 压缩节流改为方案B

### 目标
溢出时持续压缩直到未压缩活跃段可装进预算（hasOlder=false），摘要贴合当前；有迭代上限防死循环。

### 现状关键代码
- `ChatHandler.cpp:17-59` `tryCompaction`：节流 `messagesSince < 20 → return`；一次只压一轮 `kCompressChunk=40`。
- `ChatHandler.cpp:208-209`：回答落库后 `if (hasOlder && aiMsgId > 0) tryCompaction(...)` —— 每请求最多触发一次。
- `AppConfig.h:47` `summaryMinNewMessages=20`。

### 任务分解

| # | 任务 | 具体工作 | 涉及文件 | 估时 |
|---|---|---|---|---|
| P0-1 | 节流策略可配置化 | `AppConfig` 新增压缩模式/阈值：`compaction.mode = "until_fit"("throttled")`、`max_compress_rounds=5`、保留 `min_new_messages`；`tryCompaction` 据此分支 | `AppConfig.h/.cpp`、`config.*.json`、`ChatHandler.cpp:21-22` | 0.5h |
| P0-2 | 单请求内循环压缩 | 把"重算 is-overflow(hasOlder)"提为工具函数（预算口径与 `loadHistoryByTokens` 一致）；`tryCompaction` 改为 `do { 压一批 } while (hasOlder && rounds<max)`；每轮压缩后重读 hasOlder，`rounds++`；溢出到 0 则停 | `ChatHandler.cpp:17-59` | 0.5–1d |
| P0-3 | 防死循环与日志 | 迭代达上限打 warning；每轮压缩 `tprintf` 进度（批 id 区间/条数/摘要字数）；`summary_upto` 语义与 `clearContext` 不受影响 | `ChatHandler.cpp` | 0.25d |
| P0-4 | 回归测试 | 长会话(>窗口预算)单请求后消息表收敛；极小 budget 场景不卡死；`throttled` 模式行为与旧版一致；`/api/convs/clear` 后正常 | 手工+脚本构造 | 0.5d |

**验收**：溢出会话一次请求后 `messages` 条数落回"预算可装下"规模；无死循环；模式开关生效。

**风险/注意**：循环阶段连续多次调用 `LlmGateway::summarize`（模型调用）会占 worker——上限默认 5 批；DB 单连接串行无并发写问题。

---

## 2. P1-A：需求二 文档上传 → RAG + RAG GRAPH

### 目标（FR2-1~2-9）
上传→切块→embedding→入库；问答向量检索注入；入库固定生成 GRAPH，`use_graph` 可选。

### 选型落点（§22）
- D1：embedding 走 Ollama `/api/embed`，**独立于聊天上游配置** → 新增 `AppConfig.embed.*`（host/port/model= nomic-embed-text，与聊天 upstream 解耦）。
- D2：faiss 进程内 `IndexFlatIP`（归一化后=余弦）+ MySQL 存 chunk 元数据。
- D3：入库后台任务走 LLM 抽取实体/关系 → `kb_entities` / `kb_edges`。

### 数据模型草案（Db.cpp 新增建表，幂等）

```sql
CREATE TABLE IF NOT EXISTS kb_documents (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id INT UNSIGNED NOT NULL,
  title VARCHAR(200) NOT NULL,
  filename VARCHAR(255) NOT NULL,
  chunk_count INT NOT NULL DEFAULT 0,
  status ENUM('uploading','ready','failed') NOT NULL DEFAULT 'uploading',
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP, INDEX(user_id));

CREATE TABLE IF NOT EXISTS kb_chunks (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,      -- = faiss 内部 id
  doc_id BIGINT UNSIGNED NOT NULL,
  idx INT NOT NULL,
  content MEDIUMTEXT NOT NULL,
  meta_json VARCHAR(1024) DEFAULT '',                 -- 段落/页码等
  INDEX(doc_id));

CREATE TABLE IF NOT EXISTS kb_entities (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  doc_id BIGINT UNSIGNED NOT NULL,
  name VARCHAR(128) NOT NULL, type VARCHAR(32) DEFAULT '',
  INDEX(doc_id));

CREATE TABLE IF NOT EXISTS kb_edges (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  doc_id BIGINT UNSIGNED NOT NULL,
  from_entity_id BIGINT UNSIGNED NOT NULL, to_entity_id BIGINT UNSIGNED NOT NULL,
  relation VARCHAR(64) NOT NULL, INDEX(doc_id));
```

faiss 索引落盘 `data/kb.index`（`IndexFlatIP`，float32 dim=768）；向量不落 MySQL（重建凭 chunk 重新 embed）。

### 接口契约草案

- `POST /api/kb/upload`（multipart；文件名/title）→ `{code, doc_id}`；异步：解析→切块→embed→构图（status 流转）。
- `GET /api/kb/list`、`POST /api/kb/delete {id}`、`GET /api/kb/status {id}`。
- `POST /api/chat` 增可选参数：`use_graph?: bool`、`use_rag?: bool`（默认开，粒度到请求级）。
- 响应 augment：`sources: [{doc_id, chunk_idx, content_excerpt}]`。

### 任务分解

| # | 任务 | 具体工作 | 估时 |
|---|---|---|---|
| A-0 | EmbedClient | `service/EmbedClient`（仿 LlmGateway 连接逻辑，POST `/api/embed` 拿向量）；封装 batch；错误处理 | 1d |
| A-1 | 文档解析 | 解析器接口 `DocumentParser`：txt/md 直读；pdf/docx 引入轻解析（pdf 用 poppler/mupdf，docx 用 zip+XML），未支持格式返回明确错误 | 2d |
| A-2 | 切块 | `TextChunker`：按段落+长度(默认 512)+重叠(64)；返回 chunks | 0.5d |
| A-3 | 入库管线 | `KnowledgeService`：上传→解析→切块→embed→写 kb_chunks/faiss→构图（LLM 抽取：入 system"知识抽取器"提示，非流式 `LlmGateway::summarize` 同款通道；限时/限条数）；status 状态机 | 3d |
| A-4 | 检索注入 | 检索：query embed → faiss search top-k → 拼"参考资料"→ 注入 system 尾部（配额 ≤512 tok，从 usable 预算内扣）；响应带 sources | 2d |
| A-5 | 图检索 | FR2-6/7：实体邻域检索（命中 entity→取 1-hop edges→附带入参考资料）；`use_graph` 开关生效注入差异 | 2d |
| A-6 | Router/Handler | `KnowledgeHandler` + `/api/kb/*` 路由开放鉴权；chat 参数透传 | 1d |
| A-7 | 前端 | 上传页、库列表、chat 请求带 `use_graph/use_rag`、sources 展示 | 2–3d |

**验收**：上传 md → 问答命中并带 sources；DELETE 后检索不再命中；`use_graph=false` 前后回答有可观察差异；faiss 索引重启可恢复（落盘加载）。

---

## 3. P1-B：需求三 语音 ASR + TTS

### 目标（FR3-1~3-4）
录音→转文字进对话；回答朗读（流式逐句 v1.1）。

### 落点（§22 D4）
v0：浏览器 API（识别 `webkitSpeechRecognition` lang=zh-CN；合成 `speechSynthesis`，OS 离线语音）。v1：后端 ASR whisper + piper。
**设计抽象**：前端 `VoiceProvider` 接口（`browser` | `server`），配置可切；为 v1 留 `POST /api/asr` 契约。

### 接口契约
- `POST /api/asr`（v1，multipart/raw audio）→ `{code, text}`（v0 前端后端两端为空时前端自动降级 preview）。

### 任务分解

| # | 任务 | 具体工作 | 估时 |
|---|---|---|---|
| B-0 | 录音模块 | MediaRecorder：开始/结束/取消/时长与音量提示；音频格式约定 | 1d |
| B-1 | 识别集成（browser） | `SpeechRecognition` 封装进 `VoiceProvider`；识别文本回填输入框（可编辑后再发送）；不可用/网络失败时提示 & 降级 | 0.5d |
| B-2 | 朗读 TTS | `speechSynthesis` 播放回答；回答结束后读全文（v1.1 做 SSE 逐句）；暂停/继续/停止/语速；用于朗读开关持久化 localStorage | 0.5–1d |
| B-3 | UI 打磨 | 按钮态、错误 toast、移动端布局 | 0.5d |

**验收**：录音→中文文本→编辑→发送；回答可读、可中断；断网时（离线语音）朗读仍可用、识别降级提示。

---

## 4. P1-C：需求十八 历史会话召回向量化（路径B）

### 目标（FR18-1~18-4）
把 `searchMessages`/`joinKeyboardRecall` 的 LIKE 召回替换为向量召回（LIKE 兜底），**不涉及需求四文档召回**。

### 设计
- 消息向量索引：`data/history.index`（IndexFlatIP，dim=768），每条 = (conv_id<<32|msg_id) 作 faiss id。
- 全局索引 + **conv_id 过滤**（faiss `IDSelector` 位图过滤本会话），避免每会话一个文件。
- 落库时机：`appendMessage` 成功后异步 embed+写索引（失败不影响主流程，日志告警）；启动时对存量消息一次性回填（离线任务）。
- 检索：提问 embed → 过滤本 conv → top-k(6) → 预算守卫沿用 `joinKeyboardRecall`（`budget - tok >= 500`）→ 无命中/相似度低于阈值回退 `searchMessages` LIKE。

### 数据
`message_vec` 表可选（记录向量便于一致性校验/重建）；确定设计为：faiss 落盘 + 无重复 SQL 副本，重建脚本从 messages 表重放。

### 任务分解

| # | 任务 | 具体工作 | 估时 |
|---|---|---|---|
| C-0 | 消息向量化落库 | `appendMessage` 后异步 embed 写 `history.index`（复用 A-0 EmbedClient）；写失败降级仅日志 | 1d |
| C-1 | 检索改造 | `joinKeyboardRecall`：embed 提问→IDSelector 过滤→top-k→预算守卫；命中不达标回退 LIKE | 1.5d |
| C-2 | 存量回填 | 启动任务/接口扫全量消息批量 embed；幂等（按已索引 msg_id 断点） | 1d |
| C-3 | 开关与回归 | 请求/配置切换 `recall: vector|like`；验收：命中优于 LIKE、conv 严格隔离、注入预算守卫不回归 | 0.5d |

**验收**：同一提问向量召回相关旧消息优于 LIKE；不同会话互不串；LIKE 兜底生效。

---

## 5. 公共/横切改动

| 项 | 内容 |
|---|---|
| AppConfig 新增 | `embed.*`（host/port/model）、`kb.dir`、`recall.mode`、`compaction.*` |
| 前端 index.html | RAG 上传/开关、语音、召回开关、sources 展示（轮次改造，每块独立小步） |
| 构建 CMake | faiss 链接方式按 spike 结论；新增源文件登记 |
| 日志 | LlmGateway/EmbedClient 统一调用侧日志；每任务保留调试入口 |
| 安全 | 上传类型白名单+大小上限（如 10MB）；并发上传互斥（DB 单连接串行天然安全） |

## 6. 建议排期（P0 → P1）

```
第1周  spike(faiss构建 + embed验证) → P0(P0-1..P0-4)
第2周  P1-A: A-0/A-1/A-2 → 并行 P1-B: B-0..B-2（前端独立线）
第3周  P1-A: A-3/A-4/A-5（入库+检索+图）
第4周  P1-A: A-6/A-7 + P1-C: C-0..C-3（复用 A-0 基建）
预备  语音 v1 本地引擎(whisper+piper) 8-10人日 独立跟进
```

**里程碑检查点**：M2（需求二+十八）完成并回归后，再开 M3（Agent 循环）——按 §23 关键路径衔接。

## 7. 主要风险

| 风险 | 缓解 |
|---|---|
| faiss 编译/链接在 WSL 有坑 | spike 提前做；备选 apt 预编译包 |
| 上传解析（pdf/docx）增依赖与体积 | v0 先支持 txt/md，pdf/docx 排 v1.1 |
| nomic 中文效果一般（DW词向量维度 768，中文略弱于 bge） | D1 已定 nomic；若实测中文召回差，回溯升级 bge-m3（同 Ollama /api/embed 换 model 即可） |
| 向量召回命中少导致体验反而差 | LIKE 兜底 + `recall: vector|like` 开关可回退 |
| 语音识别国内不可用 | 前端降级提示 + v1 本地 whisper 接口契约已预留 |

---

*计划随实现回滚更新；每个任务开工时再生成其详细实施步骤。*