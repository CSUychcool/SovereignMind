<div align="center">

# 🚀 SovereignMind

**自托管·本地 GPU·小模型友好的 AI 助手** — 前后端一体，C++17 自研服务器 + 单文件前端，零第三方前端依赖。

云端能力你都有，但它**跑在你自己的机器上**。

同一局域网内，一台电脑就是全家共享的**端侧 AI 大脑**——Web 页面先天多端，后续移动/桌面客户端与端侧设备共享同一套模型（规划中，见「产品化路线」）。

</div>

---

## ✨ 核心优势

### 1. 为「小模型」而生的上下文管理
本地模型窗口小、预算紧，我们把它榨到极致：

- **Token 预算装历史**：按 `窗口 − 输出预留` 逐条裁剪历史，从最新往最旧装，绝不超窗被上游静默截断；
- **滚动摘要 · until_fit 连压**：溢出时单请求内循环压缩最早一批旧消息生成摘要，**连压到剩余历史装得下为止**，摘要始终贴合最新对话；
- **历史语义召回（向量优先）**：窗口外的旧消息按提问做**向量检索**召回精简注入，LIKE 关键词留作兜底；
- **上下文用量实时条**：前端按与服务端同口径的 token 估算实时显示占用，可一键 `/api/convs/clear` 清空上下文（保留对话壳）。
- 采用分段的记忆策略，让 7B/8B 级别的模型也能维持数万条对话的「记忆」。

### 2. RAG 知识库 + 知识图谱（GRAPH），开关对你开放
- 文档上传（txt/md）→ 切块 → **本地 embedding 向量化**（Ollama `/api/embed`，与聊天后端解耦）→ 入库；
- 入库时**固定生成 RAG GRAPH**（LLM 抽取实体/关系，存 `kb_entities/kb_edges` 邻接表）；
- 提问时语义检索相关片段 + 邻域图谱注入，回答带可溯源引用；
- **开发者可随时通过请求参数开关切不切图**：`use_rag` / `use_graph`（请求级或全局默认）——图照常生成，用不用由你定。

> **为什么检索选 RAG 而不是 grep？** Claude Code 这类跑强大 API 模型的工具检索用 grep——强模型能从精确命中里自己归纳整理，关键词够用就行。本地 7B/8B 小模型不同：把命中整段塞进窗口，有效信息密度低、预算浪费快。RAG 先用 embedding 做**语义召回**，只注入最相关的精简片段，对小模型更友好、效果更稳。
>
> **为什么 `use_graph` 默认关？** 一是小模型抽取实体/关系的能力有限，离线构图本身就有噪声；二是本地部署资源优先，图谱没上专门的图数据库，而是用 **MySQL 邻接表**（`kb_entities` / `kb_edges`），多跳查询靠逐跳 JOIN。对「整体架构是怎样的」这类单跳/全局提问，开图反而可能注入噪声、拉低回答质量——所以图照常生成，但默认不注入，按需打开。

### 3. 语音全链路 · 断网也有降级策略
一句话，从「说」到「听」全部本机可跑：

- **ASR（说→字）**：浏览器实时识别优先，失败自动回退**本机 whisper.cpp**（base 模型），断网可用；
- **TTS（字→听）**：**三级降级** —— `CosyVoice2`（GPU，720p 级自然度）→ `piper`（纯离线轻量）→ 浏览器系统语音；
- **SSE 流式逐句**推送音频，浏览器 WebAudio 顺序播放，支持**暂停/继续/调速**，回答完成后可自动朗读。

### 4. 接阿里开源 CosyVoice2 · 可定制音色
- 基于阿里开源 **CosyVoice2-0.5B** 作为 GPU TTS 首选，内置多音色可选，零样本路径可扩展到**音色克隆**；
- 模型常驻 GPU sidecar（127.0.0.1:9101），逐句合成毫秒级响应，不阻塞主服务。

---

## 🌐 局域网端侧互联：一台电脑，全家智能

产品功能在局域网内天然互通。**一台性能足够的电脑**运行 SovereignMind，就是全屋/全办公室的**端侧 AI 大脑**：

- **多端共享**：Web 页面先天跨端；前端轻量、主体实现全在服务端，任何设备打开浏览器即接入同一套模型与会话；
- **设备直达**：小型/嵌入式设备（智能音箱、传感器网关、嵌入式终端）**无需自带模型**，经网关直达服务端发送消息/查询，共享端侧 AI 模型实现智能化；
- 面向端侧设备的轻量 API（HTTP + WebSocket、设备级 token 鉴权）已列入需求文档（`docs/requirements.md` 需求十九）。

---

## 🏗 架构

```
index.html (单文件前端 · 深空主题)
   └─ http://localhost:9000 → llm-server (C++17 自研事件驱动 HTTP)
              │  ├─ /api/chat      → 上游 Ollama(11434) / vLLM(8000) SSE 流式
              │  ├─ /api/auth/*    → 注册/登录/Token 会话 (MySQL)
              │  ├─ /api/convs*    → 对话/消息 CRUD + 清上下文 (MySQL)
              │  ├─ /api/kb/*      → RAG 知识库: 上传/列表/删除 (faiss/向量 + MySQL)
              │  ├─ /api/control/* → 模型状态/一键切换
              │  ├─ /api/tts       → 本地 TTS: SSE 逐句音频 (CosyVoice→piper→系统)
              │  ├─ /api/asr       → 本地 ASR: wav→文本 (whisper.cpp)
              │  └─ GET /          → index.html (web_root)
   upstream 模型:  Ollama:11434  ·  vLLM:8000   (可切换)
   语音引擎:       Ollama /api/embed (nomic-embed-text)
                   CosyVoice2 sidecar:9101 · piper · whisper.cpp
```

平台分层（依赖单向）：`net/`(双线程池+fd移交) → `route/`(集中鉴权) → `handler/` → `service/`(领域) → `db/`(MySQL)。

---

## 🔌 特性一览

- SSE 流式回复 · Markdown 渲染 · 代码一键复制
- 服务器端账号：注册/登录/Token 会话，密码加盐哈希落库
- 对话与消息全存 MySQL，刷新不丢、多端同步；多会话列表、自动命名、一键导出
- **上下文管理**：预算装历史 / 滚动摘要 until_fit / 历史向量召回 / 用量条 / 一键清空
- **RAG**：知识库上传、语义检索注入、**RAG GRAPH 可选使用**（`use_graph`）
- **语音**：录音转文字（浏览器→whisper 降级）、逐句朗读（CosyVoice→piper→系统降级）、暂停/调速/自动读
- 模型一键切换（Ollama ↔ vLLM，自动重启服务不丢会话）
- 同源 API 自适应：本地 `file://` 打开或 frp/隧道公网 HTTPS 均免配置
- **局域网端侧互联**：一台电脑即端侧 AI 大脑，多端/设备共享同一模型；客户端 · TCP 穿透 · Docker 见「产品化路线」

---

## 🗺 产品化路线（规划中）

详见 `docs/requirements.md` 需求十九~二十三。

| 方向 | 一句话 | 关键点 |
|---|---|---|
| 🖥️ **端侧 AI 设备接入 API** | 局域网内一台电脑做大脑，嵌入式/智能设备经网关直达共享模型 | 设备级 token 鉴权、HTTP+WS 双通道、弱网适配（需求十九） |
| 📱 **移动端 App（APK/iOS）** | WebView 壳封装现前端，先天跨端、随时随身 | 主体在服务端、前端轻量，打包成本极低（需求二十） |
| 🖥️ **桌面端 App（Qt 跨端）** | 服务端本就是 C++17，Qt 天然覆盖 Windows/macOS/Linux | 可先 WebEngine 壳，再演进原生客户端（需求二十一） |
| 🔌 **软件内 TCP 内网穿透（免备案）** | 网页穿透只对备案域名提供 HTTP/HTTPS；**App 内走 TCP 穿透免备案** | 客户端先探局域网直连，失败自动切穿透（需求二十二） |
| 🐳 **一键 Docker 部署** | 环境与模型依赖多，打成镜像 + compose 一条命令交付 | 多阶段构建、数据卷持久化、GPU 可选（需求二十三） |

> 为什么"打包成软件"这么值？网页形态的外网访问，内网穿透只对**备案过的网址**提供 HTTP/HTTPS 服务；打进 App/客户端后，可以通过 **TCP 形式**实现内网穿透，**无需备案网页**（需求二十二）。另一个收益：**Docker 化**把繁琐的环境/模型依赖变成一键交付，私有化分发成本大降（需求二十三）。

---

## 🧰 技术栈

C++17 · 自研事件驱动 HTTP（Epoll + 双线程池）· jsoncpp · MySQL 8 · OpenSSL · 原生 JavaScript
语音：whisper.cpp · piper · CosyVoice2(可选 GPU) · 向量：进程内向量索引（预留 faiss 后端）

---

## 🛠 构建

依赖：`g++ / cmake / libmysqlclient-dev / libjsoncpp-dev / libssl-dev`

```bash
cmake -S llm-server -B build
cmake --build build -j4
```

> `llm-server/CMakeLists.txt` 的 `EXECUTABLE_OUTPUT_PATH` 保留了本机路径 `/home/yc_21/server_ddz/bin`，克隆后按需修改。

## ▶️ 运行

1. 准备 MySQL 库与账号，复制 `config.ollama.example.json` 为 `config.ollama.json` 填 `db` 段；首次启动自动建表；
2. 启动上游 `ollama serve`（或 vLLM）；拉取 embedding 模型 `ollama pull nomic-embed-text`；
3. 启动服务：`bin/llm-server --config llm-server/config.ollama.json`（默认 9000）；
4. 安装语音引擎（可选，但推荐）：
   ```bash
   bash voice/setup.sh                 # piper 预编译 + 中文音色 + whisper.cpp 构建
   bash voice/setup_cosyvoice.sh       # conda env + CosyVoice 依赖 + modelscope 拉模型
   python voice/cosyvoice_server.py --port 9101 --model voice/cosyvoice-model/CosyVoice2-0.5B
   ```
5. 打开 `index.html` 注册使用。

---

## 🛡 安全提醒

暴露公网前（frp/隧道）确保：所有 `/api/*` 均要求登录（Token）；建议再加一层 Basic Auth / IP 白名单。本项目为私有自托管定位，未内置联邦登录。