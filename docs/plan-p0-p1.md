# P0 / P1 具体实施计划

- 关联文档：`docs/requirements.md` v0.6（需求基线 & 选型结论 §22 & 总体计划 §23）
- 日期：2026-09-09
- 分支：main（featuer/p0-p1 已于 2026-09-10 合入）
- 说明：本文档给出 **P0 与 P1 的任务级计划**（每个任务=一次可开工实现的单位）。进一步到"某个具体任务的实施步骤"仍按约定在真正运行时再拆。
- 状态（2026-09-10）：**P0/P1 已全部实现并入 main**；V1 语音(ASR/TTS) 后端已并入，CosyVoice2 音色升级安装中。

## 实现状态核对（2026-09-10，按实际代码复核）

> 与计划有出入处**以实际代码为准**。实现已随 `featuer/p0-p1 → main` 合入。

| 计划项 | 实际状态 | 与文档的差异 |
|---|---|---|
| P0 节流B（`until_fit` 连压 + `maxCompressRounds=5` + `throttled` 可切） | ✅ 已实现并验收 | 无 |
| P1-C 历史会话向量召回（路径B） | ✅ 已实现（`recall_mode=vector`；`ensureMsgVectors` 增量回填；向量优先 + LIKE 兜底） | 无 |
| 向量库（计划 D2=faiss） | ⏳ **当前=进程内暴力余弦**：向量存 MySQL `vec_index`（BLOB float32），`VectorStore` 接口化 | **偏差：faiss 未落地**（WSL 无 sudo），接口可无缝替换 |
| RAG 上传/入库/检索注入 | ✅ 已实现并验收（upload/list/delete；`use_rag/use_graph` 请求开关；GRAPH=LLM 抽取→kb_entities/kb_edges） | **上传=JSON `{title,filename,content}`**（原文档写 multipart）；**无 `/api/kb/status`** |
| RAG 引用 sources | ❌ **未实现**（按 FR2-9 归 P2；服务端/前端均无 sources） | 文档 A-4/A-7 的"响应带 sources/前端展示"未做 |
| 语音 P1-B 前端 | ✅ 已实现（录音/朗读/暂停调速/自动朗读/设置项） | **识别=浏览器优先→失败回退本地 whisper**（按用户要求反转；原文档是"本地优先浏览器兜底"） |
| V1 语音后端 | ✅ 代码已并入（`/api/tts` SSE、`/api/asr` whisper.cpp） | — |
| TTS 引擎 | 🔄 `tts_backend=cosyvoice` 首选（sidecar 就绪），**引擎安装进行中**；piper 已就绪作兜底 | **新增**（原文档 V1 定为 piper） |
| 语音引擎 | ✅ piper 预编译 + 中文音色、whisper.cpp(base 142MB) 已构建 | — |

## 语音 V1 最终方案（2026-09-10，含"为什么"）

> 为小模型/本地 GPU 设计的语音链路：**ASR whisper + TTS CosyVoice2**，全本机、断网可用、内容 100% 对齐。

### 链路与分层

```
🎤 录音 → 浏览器识别优先 → 失败回退 /api/asr(whisper.cpp, -l zh)
🔊 /api/tts(9000) → 整段文本一口交给 sidecar → CosyVoice2(GPU, cross_lingual) → SSE 音频 → 浏览器播放
   兜底链: cosyvoice → piper(离线) → 系统语音;  音色: 前端下拉(cosyvoice-voices/*.wav+.txt)
```

### 关键设计决策与原因

| 决策 | 为什么 |
|---|---|
| **引擎 = CosyVoice2-0.5B**（sidecar :9101 常驻 GPU） | 阿里开源、本地 GPU 推理、零样本可克隆音色；劣于商业云端/edge-tts，但满足"自托+按捂定音色" |
| **推理用 `cross_lingual`，不用 `zero_shot`** | zero_shot 会把 **prompt 文本续说成开场白**（林志玲/自录音均复现：先说"微笑面對生活…世界的美好"再接正文，或整段随机乱码）→ cross_lingual 只把 **prompt 音频特征**给 LLM、**不喂 prompt 文本**，内容严格跟随目标文本、音色不变。**这是"朗读内容不干净/像方言"的根治点** |
| **采样压到 `top_p=0.15/top_k=5`** | zero_shot/LLM 每步 `multinomial` 随机采样 → 同文本不同乱码；压采样趋近贪心 + ras 重复惩罚兜底 → 内容确定性对齐目标文本（0.8/25→乱、0.4/12→部分前缀、0.15/5→干净） |
| **整段一口合成**（sidecar 内部分句，一次推理整段返回） | 每句单独调一次 GPU → 句间等数秒、明显断裂；一口合成后前端连续播放，无间隔 |
| **朗读文本用 `dataset.raw` + `plainForTts`，不取渲染 DOM** | `msg-bubble.textContent` 混入"复制"按钮/代码语言标签等 → 朗读不干净 |
| **音色做成"库"**：`cosyvoice-voices/<name>.wav + .txt`、`GET /voices`、前端下拉 | 一个 wav+txt=一个音色的可插拔音色墙；prompt 音频需**单句、干净**（带 BGM/混响的样本会让特征提取失败→乱码；动过 `silenceremove`+高通后 13-01 可用） |
| prompt 特征缓存默认**关**（`VOICE_CACHE=1` 可开） | 缓存能省每句 CPU 特征提取，但若与官方路径有偏差会引入内容误差；**先保正确性，缓存当性能选项** |
| **ASR 强制 `-l zh`** | whisper base 自动语种会把中文合成音误判英文 |
| 前端 TTS 引擎三档：本地(CosyVoice→piper)/系统；音色两档：林志玲/我的声音 | 让用户有"可靠默认 + 特效音色"的可选空间 |

### 验证（自动化 whisper 回读对账）

| 音色 | 目标文本 | whisper 回读 |
|---|---|---|
| linzhi(13-01 预处理) | 请用中文简单介绍一下二分查找，它有哪些适用条件？ | "请用中文简单介绍一下二分查找,它有哪些试用条件" ✅ |
| me(自录 15s) | 同上 | "请用中文简单介绍一下二分查找,它有哪些试用条件。" ✅ |

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

### 前置 spike 结论（2026-09-09 已跑，→ P1-A 依赖项）

- **faiss**：Ubuntu 24.04 `apt-cache search faiss` 无结果 → 需 `apt-get update` 后 `apt-get install -y libfaiss-dev`（可能需 sudo），或源码编译。**P1-A 开工前置，待 M2 敲定装法**。
- **Ollama /api/embed**：接口可达；当前缺 `nomic-embed-text` 模型，需 `ollama pull nomic-embed-text`（约 270MB）后再验证维度/中文效果。
- **模型工具能力（D5）**：Ollama 现有 `qwen3:8b`（tools+thinking）与 `qwen2.5:7b-instruct-q4_K_M`（tools）→ M3"原生优先"策略可用面宽，文本指令式兜底仍保留。

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
- D2：向量库 —— **实际落地 v0=进程内暴力余弦**（`VectorStore`，等价 IndexFlatIP 语义），向量存 MySQL `vec_index`(kind/group/item, BLOB)；faiss 后装后经 `VectorStore` 接口替换（无需改调用方）。
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

向量实际存放：**v0=MySQL `vec_index` 表 BLOB**（float32，归一化向量，内积即余弦）；faiss 索引落盘方案（`data/kb.index`）为 faiss 后端落地后的形态。

### 接口契约草案

- `POST /api/kb/upload`：**JSON `{title?, filename, content}`**（txt/md 内容直传）→ `{code, doc_id}`；流程：解析→切块→embed（Ollama /api/embed）→写 `kb_chunks`+`vec_index`→GRAPH 抽取入库（同步完成）。
- `GET /api/kb/list`、`POST /api/kb/delete {id}`（已实现）；`/api/kb/status` **未实现**。
- `POST /api/chat` 已支持 `use_graph?: bool`、`use_rag?: bool`（默认开，请求级）。
- 引用来源 `sources` **未实现**（归 P2，FR2-9）。

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
实现（2026-09-10 已定稿）：**识别=浏览器 `SpeechRecognition` 优先（实时转写），失败/不支持回退本机 whisper（/api/asr）**；朗读走 `/api/tts` SSE（后端 `tts_backend=cosyvoice`→piper 兜底），前端 WebAudio 队列播放 + 暂停/调速。`VoiceProvider` 抽象保留（browser | server）。

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