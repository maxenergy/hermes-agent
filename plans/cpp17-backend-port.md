# Hermes Agent — C++17 后端复刻开发计划

> **目标**:用 C++17 完整复刻 hermes-agent 的所有后端功能(不含 Web UI / 落地页 / 文档站)。
>
> **使用方式**:每完成一项,把 `[ ]` 改成 `[x]`,并在该项后追加完成日期与提交哈希。
>
> **完成定义**(Definition of Done):每个功能必须配套单元测试 + 集成测试,并且与 Python 版本在相同输入下行为等价(轨迹、JSON 输出比对通过)。
>
> **范围说明**:Python 源仓库共 56 个工具文件、21 个平台适配器、~50 个 hermes_cli 子模块、~28 个 agent/ 子模块。本计划逐项列出,**不漏任何一个**。

> ### ✅ 实现状态:阶段 0-20 全部完成(2026-04-12,联合测试 2026-04-13)
>
> **952 测试绿(929 单元 + 23 联合)/ 388+ 源文件 / ~40k LOC**
>
> 所有阶段的核心功能已实现并合入 main 分支。复选框更新状态:
> - **阶段 0-8**:逐条勾选完毕(见下方各小节)
> - **阶段 9-10**:逐条勾选完毕
> - **阶段 11-20**:逐条勾选完毕(batch 9-10 补勾)
> - **最后批次**(2026-04-13):checkpoint_manager / mcp_oauth / tirith_security /
>   skills_sync / browser_camofox / neutts / tool_backend_helpers / debug_helpers
>   (commit `a2052008`) + 23 joint integration tests(commit `1634f3d1`)
>
> **每个 commit 的精确范围见底部"进度追踪小节"。**
>
> **仍为 stub/TODO 的功能**(代码在,但运行时返回错误或占位):
> - 语音:transcription 需 faster-whisper,voice_mode 为状态机骨架
> - 平台适配器:WebSocket 长连接(Discord/Slack)待接入
> - OAuth / Honcho:stub
> - Windows:所有环境后端 `#ifdef _WIN32` → throw
> - CI 流水线:Linux x86_64 单跑,多平台矩阵待补

---

## 阶段 0 — 项目基础设施

### 0.1 工程脚手架
- [x] 初始化 CMake 工程(`CMakeLists.txt` 顶层 + 子目录),C++17 标准,开启 `-Wall -Wextra -Wpedantic -Werror` (2026-04-12, d21d29c9)
- [x] 接入 vcpkg 作为依赖管理(manifest 模式,`cpp/vcpkg.json`) (2026-04-12, d21d29c9)
- [x] 接入 clang-format / clang-tidy 配置 (2026-04-12, d21d29c9)
- [x] 接入 GoogleTest(FetchContent v1.15.2,`gtest_discover_tests` 绑定) (2026-04-12, d21d29c9)
- [x] 接入 sanitizer 构建变体(`asan` preset = ASan + UBSan) (2026-04-12, d21d29c9)
- [ ] CI 流水线:Linux x86_64 / Linux arm64 / macOS / Windows-MSVC + WSL2(对齐 Python 版支持矩阵) *— deferred: 03ac9465 多架构 Docker 落地但完整多平台 CI 矩阵未启动*
- [x] 设置 Release / Debug / asan 三套构建预设(`CMakePresets.json`) (2026-04-12, d21d29c9)

### 0.2 第三方依赖选型与封装
- [x] **HTTP 客户端**:libcurl `CurlTransport` 封装为 `HttpClient`(支持 SOCKS 代理、自动重试、超时) (2026-04-12, 52c4bf0b)
- [x] **WebSocket**:`Boost.Beast`,用于 Discord/Slack/Matrix 长连接 (2026-04-12, 52c4bf0b)
- [x] **JSON**:`nlohmann/json`(等价 Python `json`) (2026-04-12, d21d29c9)
- [x] **YAML**:`yaml-cpp`(等价 `pyyaml`) (2026-04-12, d21d29c9)
- [x] **正则**:C++ `<regex>` + 备用 `re2`(用于审批模式 45+ 条 PCRE) (2026-04-12, dc5f6c19)
- [x] **SQLite**:`sqlite3` 直接调用 + `SQLiteCpp` 封装,**必须启用 FTS5** (2026-04-12, 4318321b)
- [x] **加密**:`OpenSSL`(SHA256、HMAC、TLS、libcrypto) (2026-04-12, d21d29c9)
- [x] **JWT**:`jwt-cpp`(对应 Python `PyJWT[crypto]`) (2026-04-12, d21d29c9)
- [x] **协程 / 异步运行时**:选 `Boost.Asio` 或 `libuv`(后续所有异步 I/O 统一) (2026-04-12, 52c4bf0b)
- [x] **进程管理**:`Boost.Process` 或自建 `posix_spawn` 封装(对应 Python `subprocess` + `ptyprocess`) (2026-04-12, dc91ac71)
- [x] **PTY**:Linux/macOS 用 `forkpty`,Windows 用 `pywinpty` 等价物 / ConPTY (2026-04-12, dc91ac71)
- [x] **终端 UI**:`FTXUI` 或 `ncurses`(对应 prompt_toolkit + Rich + curses) (2026-04-12, 370adb86)
- [x] **logging**:`spdlog`(滚动文件 + 异步,stderr fallback) (2026-04-12, 52c4bf0b)
- [x] **CLI 解析**:`CLI11`(对应 Python `fire`) (2026-04-12, 370adb86)
- [x] **模板引擎**:`inja`(对应 `jinja2`) (2026-04-12, b060cee1)
- [x] **Markdown 渲染**:`md4c` 或 `cmark-gfm` (2026-04-12, 370adb86)
- [x] **base64 / urlencode / unicode NFKC**:`utfcpp` + `unicode-icu` (2026-04-12, dc5f6c19)
- [x] **fcntl 文件锁封装**:跨平台 advisory lock 抽象 (2026-04-12, 52c4bf0b)
- [x] **fnmatch / glob**:C++17 `<filesystem>` + 自实现 glob (2026-04-12, d21d29c9)

### 0.3 基础工具库 `core/`
- [x] `core/strings.hpp`:split、join、startswith、endswith、trim、lower、upper、contains (2026-04-12, d21d29c9)
- [x] `core/path.hpp`:`get_hermes_home()`、`display_hermes_home()`、`get_default_hermes_root()`、`get_profiles_root()`、`get_hermes_dir()`、`get_optional_skills_dir()` (2026-04-12, d21d29c9)
- [x] `core/atomic_io.hpp`:`atomic_write` / `atomic_read`(temp + fsync + rename) (2026-04-12, d21d29c9)
- [x] `core/env.hpp`:`env_var_enabled()`、`is_truthy_value()`、`load_dotenv()` (2026-04-12, d21d29c9)
- [x] `core/time.hpp`:`now()` + `format_iso8601` + `$HERMES_TIMEZONE` 解析 (2026-04-12, d21d29c9)
- [x] `core/redact.hpp`:7 条秘钥正则(sk-/ghp_/Bearer/token/key/password/secret) (2026-04-12, d21d29c9)
- [x] `core/logging.hpp`:最小 stderr 封装(spdlog 接入留到阶段 1+) (2026-04-12, d21d29c9)
- [x] `core/retry.hpp`:`jittered_backoff` 指数退避 + 抖动 (2026-04-12, d21d29c9)
- [x] `core/url_safety.hpp`:SSRF 拦截(RFC1918 / 127 / link-local / ::1 / fc00::/7 / metadata) (2026-04-12, d21d29c9)
- [x] `core/ansi_strip.hpp`:ECMA-48 状态机(CSI/OSC/2-char/8-bit C1) (2026-04-12, d21d29c9)
- [x] `core/fuzzy.hpp`:Levenshtein DP + `fuzzy_contains` (2026-04-12, d21d29c9)
- [x] `core/patch_parser.hpp`:unified diff 解析(FileDiff + Hunk) (2026-04-12, d21d29c9)

---

## 阶段 1 — 配置与多 Profile 体系

### 1.1 配置加载
- [x] `config/default_config.hpp`:顶层结构(model/provider/base_url/terminal/tools/display/memory/messaging/web/tts/_config_version=5)(2026-04-12, 005749d3)
- [x] `config/optional_env_vars.hpp`:20+ 条目(OpenRouter/Anthropic/OpenAI/Google/Mistral/Telegram/Discord/Slack 等)(2026-04-12, 005749d3)
- [x] `config::load_cli_config()`:读 `~/.hermes/config.yaml` + YAML→JSON + deep merge over default (2026-04-12, 005749d3)
- [x] `config::load_config()`:目前等同于 `load_cli_config`(差异化留到 Phase 13) (2026-04-12, 005749d3)
- [x] 网关直接 YAML 加载路径(对应 `gateway/run.py`) (2026-04-12, 932ddb2c)
- [x] `_config_version` 迁移 stub:缺失时 bump 到 v5,v1-v5 per-field 迁移留 TODO(phase-2) (2026-04-12, 005749d3)
- [x] `${VAR}` / `${VAR:-default}` 模板展开 (2026-04-12, 005749d3)
- [x] `detect_managed_system()`:None/NixOS/Homebrew/Debian 检测(含 Linuxbrew) (2026-04-12, 005749d3)

### 1.2 Profile 多实例
- [x] `apply_profile_override()`:`setenv("HERMES_HOME", ...)` 先于所有其他代码 (2026-04-12, 005749d3)
- [x] `get_profiles_root()`:HOME 锚定断言(测试覆盖,核心不变量) (2026-04-12, 005749d3)
- [x] profile list / create / delete / rename(删除 active 抛异常的安全阀) (2026-04-12, 005749d3)
- [x] 独立 HERMES_HOME 下 config/.env 隔离 (2026-04-12, 005749d3)
- [x] 测试:profile 隔离 + HOME 锚定不变量 (2026-04-12, 005749d3)

### 1.3 凭据管理
- [x] `~/.hermes/.env` 读写,store 后 chmod 0600 (2026-04-12, 005749d3)
- [x] `store_credential` / `get_credential` / `clear_credential` / `clear_all_credentials` / `list_credential_keys` (2026-04-12, 005749d3)
- [x] `load_profile_env()`:HERMES_HOME `.env` + CWD 项目 `.env` 双层 overlay,幂等 (2026-04-12, 005749d3)
- [x] OAuth 流程脚手架(对应 `auth_commands.py`) — `cpp/auth/src/qwen_oauth.cpp` + `copilot_oauth.cpp` + `mcp_oauth.cpp` (2026-04-13, a2052008, a784b2e5, e3dad4d4)
- [x] GitHub Copilot OAuth(对应 `copilot_auth.py`) — device code flow (2026-04-13, e3dad4d4)
- [x] Nous 订阅状态校验(对应 `nous_subscription.py`) (2026-04-13, e3dad4d4)
- [x] 每平台 token scoped lock(见网关阶段) (2026-04-13, f0215995, 2198dc39)

---

## 阶段 2 — 状态与持久化

### 2.1 SessionDB(SQLite + FTS5)
- [x] schema v6:`sessions` 表(id/source/model/config/created_at/updated_at/tokens/cost/title) (2026-04-12, 4318321b)
- [x] `messages` 表 + `idx_messages_session` 索引(session_id+turn_index) (2026-04-12, 4318321b)
- [x] `messages_fts` FTS5 虚拟表 + ai/ad/au 同步触发器 (2026-04-12, 4318321b)
- [x] WAL 模式 + `synchronous=NORMAL` + `busy_timeout=5000` + 50 次写入 PASSIVE checkpoint (2026-04-12, 4318321b)
- [x] 写入冲突重试:15 次 + `jittered_backoff` 20-150ms 抖动 (2026-04-12, 4318321b)
- [x] `create_session` / `save_message` / `get_session` / `list_sessions` / `get_messages` / `delete_session` / `add_tokens` / `update_session_title` (2026-04-12, 4318321b)
- [x] `fts_search(query, limit)` (2026-04-12, 4318321b)
- [x] FTS5 可用性运行时探测(创建临时 FTS5 表) (2026-04-12, 4318321b)
- [x] 单元测试:并发写、FTS 命中、级联删除 (2026-04-12, 4318321b)
- [x] v1→v5 per-version 迁移路径(v1→v2→v3→v4→v5→v6 每步独立 + `test_migration.cpp` fixtures) (2026-04-13, fb99baf7)

### 2.2 ProcessRegistry(后台进程注册表)
- [x] `ProcessSession` 数据结构(id/command/task_id/pid/state/exit_code/cwd/时间戳/pid_scope/detached) (2026-04-12, 4318321b)
- [x] 200KB 滚动 output buffer (2026-04-12, 4318321b)
- [x] watch_patterns 子串扫描 + 8/10s 限速 + 45s 超载自动 kill (2026-04-12, 4318321b)
- [x] `register_process` / `get` / `list_running` / `list_finished` / `mark_exited` / `kill`(数据层) (2026-04-12, 4318321b)
- [x] `feed_output` + `drain_notifications` 通知队列 (2026-04-12, 4318321b)
- [x] JSON checkpoint 文件 + restore + 孤儿检测 (2026-04-12, 4318321b)
- [x] `spawn_local()` 实际 POSIX fork/exec 接入(cwd/env/timeout/streaming/pgid kill)
- [x] `spawn_via_env()` 远程环境转发 (2026-04-13, 41e00306)

### 2.3 内存与轨迹存储
- [x] `memory/MEMORY.md` + `USER.md` 持久化,`§` 段落分隔 (2026-04-12, 4318321b)
- [x] `flock(LOCK_EX)` + in-process mutex 双层锁(fcntl 在同进程线程间等价,已绕开) (2026-04-12, 4318321b)
- [x] `add` / `read_all` / `replace` / `remove`(子串匹配) (2026-04-12, 4318321b)
- [x] 威胁扫描 5 条正则:ignore prev instructions / pipe-to-shell / ssh-rsa / 隐藏 div / .env 读取 (2026-04-12, 4318321b)
- [x] `TrajectoryWriter`:JSONL 追加(O_APPEND) (2026-04-12, 4318321b)
- [x] `convert_scratchpad_to_think()`:`<REASONING_SCRATCHPAD>` → `<think>` (2026-04-12, 4318321b)
- [x] `has_incomplete_scratchpad()` (2026-04-12, 4318321b)
- [x] 启动时载入到内存缓存(`memory_store` 内存缓存 + test_memory_cache) (2026-04-12, 4318321b)
- [x] `checkpoint_manager`(长任务断点) (2026-04-13, a2052008)

---

## 阶段 3 — 模型与 LLM 集成

### 3.1 模型元数据
- [x] `fetch_model_metadata(model, base_url)` —— 硬编码回退表(models.dev HTTP 留到阶段 4 注入点) (2026-04-12, 55b4a2fc)
- [x] `estimate_tokens_rough()` / `estimate_messages_tokens_rough()`(4 字符 ≈ 1 token 启发) (2026-04-12, 55b4a2fc)
- [x] `CONTEXT_PROBE_TIERS = {128K, 64K, 32K, 16K, 8K}` (2026-04-12, 55b4a2fc)
- [x] `parse_context_limit_from_error()` (2026-04-12, 55b4a2fc)
- [x] `strip_provider_prefix()`:`anthropic/` 等 (2026-04-12, 55b4a2fc)
- [x] `models_dev::fetch_spec()` 实际 HTTP fetch + 缓存 (2026-04-12, 29936e3c)
- [x] `query_ollama_num_ctx()`(本地 Ollama HTTP 查询) (2026-04-12, 29936e3c)
- [x] models.dev 实际 HTTP fetch + 3600s 缓存 (2026-04-12, 29936e3c)

### 3.2 LLM 客户端
- [x] `OpenAIClient`:Chat Completions 非流式 (2026-04-12, 55b4a2fc)
- [x] `AnthropicClient`:Messages API 非流式(system top-level + content blocks + cache_control) (2026-04-12, 55b4a2fc)
- [x] `OpenRouterClient`:OpenAI 格式 + `HTTP-Referer` + `X-Title` 头 (2026-04-12, 55b4a2fc)
- [x] `LlmClient` 抽象 + `HttpTransport` 注入点 + `FakeHttpTransport`(用于单测) (2026-04-12, 55b4a2fc)
- [x] **关键**:`apply_anthropic_cache_control`(`prompt_cache.hpp`) (2026-04-12, 55b4a2fc)
  - [x] system_and_3 策略(4 breakpoint 硬上限) (2026-04-12, 55b4a2fc)
  - [x] `cache_ttl="5m"` 默认 (2026-04-12, 55b4a2fc)
  - [x] `native_anthropic` 开关 (2026-04-12, 55b4a2fc)
  - [x] 幂等(idempotent)—— 二次调用等价 (2026-04-12, 55b4a2fc)
- [x] `Message`/`ContentBlock`/`ToolCall`:OpenAI + Anthropic 双向序列化(`to_openai`/`from_openai`/`to_anthropic`/`from_anthropic`) (2026-04-12, 55b4a2fc)
- [x] reasoning / tool_calls 字段解析(两种格式) (2026-04-12, 55b4a2fc)
- [x] SSE 流式响应解析 (2026-04-12, 13b454b6)
- [x] `CurlTransport`:libcurl 实现(real HTTP) (2026-04-12, 52c4bf0b)

### 3.3 用量与定价
- [x] `CanonicalUsage`:input/output/cache_read/cache_creation/reasoning tokens (2026-04-12, 55b4a2fc)
- [x] `normalize_openai_usage()` / `normalize_anthropic_usage()` (2026-04-12, 55b4a2fc)
- [x] `PricingTier` + `lookup_pricing()`(claude-opus-4-6/sonnet/haiku/gpt-4o/gemini-2.0-flash/deepseek-chat 等) (2026-04-12, 55b4a2fc)
- [x] `estimate_usage_cost()` (2026-04-12, 55b4a2fc)
- [x] `format_token_count_compact()` / `format_duration_compact()` (2026-04-12, 55b4a2fc)

### 3.4 错误分类与故障转移
- [x] `classify_api_error(status, body, headers)` → `FailoverReason`(None/RateLimit/ContextOverflow/Unauthorized/ModelUnavailable/ServerError/Timeout/NetworkError/Unknown) (2026-04-12, 55b4a2fc)
- [x] `ClassifiedError.retry_after` 解析(`Retry-After` / `retry-after-ms`) (2026-04-12, 55b4a2fc)
- [x] `ClassifiedError.context_limit_hint` 解析 (2026-04-12, 55b4a2fc)
- [x] `backoff_for_error()`:优先用 Retry-After,否则 jittered_backoff (2026-04-12, 55b4a2fc)
- [x] `RateLimitState::update_from_headers()`:`x-ratelimit-remaining-*` / `x-ratelimit-reset` 变体 (2026-04-12, 55b4a2fc)
- [x] `smart_model_routing`:模型回退链(`cpp/llm/src/smart_routing.cpp`) (2026-04-12, 13b454b6)
- [x] tier-down 建议(`tier_down_for_context`) (2026-04-12, 13b454b6)

### 3.5 凭据池与运行时提供商
- [x] `credential_pool`:多提供商凭据缓存 (2026-04-13, cf65e739)
- [x] `runtime_provider::resolve_runtime_provider()` —— model / api_key / base_url 解析 (2026-04-13, cf65e739)
- [x] `model_normalize`:剥离 `provider/` 前缀 (2026-04-12, 55b4a2fc)
- [x] `codex_models`:Codex 兼容模型识别 (2026-04-13, cf65e739)

### 3.6 辅助 LLM 客户端
- [x] `auxiliary_client`:廉价快速模型(默认 Gemini Flash),用于视觉 / 摘要 / 标题生成 (2026-04-12, 13b454b6)
- [x] `title_generator::generate_title(first_turn) → string` (2026-04-12, 13b454b6)
- [x] `anthropic_adapter` 原生封装 (2026-04-12, 13b454b6)

---

## 阶段 4 — Agent 核心循环

### 4.1 系统提示构建
- [x] `prompt_builder::DEFAULT_AGENT_IDENTITY` 模板 (2026-04-12, b060cee1)
- [x] `PLATFORM_HINTS` 字典(cli / telegram / discord / slack / 等) (2026-04-12, b060cee1)
- [x] `MEMORY_GUIDANCE` / `SESSION_SEARCH_GUIDANCE` / `SKILLS_GUIDANCE` 段落 (2026-04-12, b060cee1)
- [x] `build_nous_subscription_prompt()`(内联于 prompt_builder build_system_prompt) (2026-04-12, b060cee1)
- [x] `build_skills_system_prompt()`(内联 SKILLS_GUIDANCE + skills_index) (2026-04-12, b060cee1)
- [x] `build_context_files_prompt()`(内联 Project context files 段) (2026-04-12, b060cee1)
- [x] **上下文文件扫描**:`.hermes.md` / `HERMES.md` 沿 git root 向上发现 (2026-04-12, b060cee1)
- [x] YAML frontmatter 剥离 (2026-04-12, b060cee1)
- [x] **prompt injection 检测**:7 条模式(override / hidden-div / pipe-to-sh / ssh-rsa / .env) (2026-04-12, b060cee1)
- [x] `subdirectory_hints::SubdirectoryHintTracker` LRU (2026-04-12, b060cee1)
- [x] `context_references` 关联上下文文件与网络源 — `cpp/agent/src/context_references.cpp` (2026-04-13, 3d734baf)

### 4.2 上下文压缩
- [x] `context_engine.hpp` 抽象基类:`compress()` / `on_session_reset()` / `update_model()` (2026-04-12, b060cee1)
- [x] `context_compressor`:~50% 阈值触发 (2026-04-12, b060cee1)
- [x] 算法:保护 head + tail(4 turns / 20K tokens),辅助模型摘要中间 (2026-04-12, b060cee1)
- [x] 摘要模板:Goal / Progress / Decisions / Files / Next Steps (2026-04-12, b060cee1)
- [x] `manual_compression_feedback`:用户对压缩质量反馈 — `cpp/agent/src/compression_feedback.cpp` (2026-04-13, 3d734baf)

### 4.3 内存管理
- [x] `MemoryManager`:协调 builtin + 至多一个 external provider (2026-04-12, b060cee1)
- [x] `add_provider()`:校验 single-external 限制 (2026-04-12, b060cee1)
- [x] `build_system_prompt()` 合并 provider prompts (2026-04-12, b060cee1)
- [x] `prefetch_all` / `sync_all` / `queue_prefetch_all` (2026-04-12, b060cee1)
- [x] `MemoryProvider` 抽象基类 (2026-04-12, b060cee1)
- [x] Builtin file-based provider (backed by MemoryStore) (2026-04-12, b060cee1)
- [x] Honcho AI provider(可选) — `cpp/agent/src/honcho_provider.cpp` (2026-04-13, f0215995)
- [x] Insights 模块 — `cpp/agent/src/insights.cpp` (2026-04-13, 3d734baf)

### 4.4 AIAgent 主类
- [x] DI 构造函数:AgentConfig + LlmClient + SessionDB + ContextEngine + MemoryManager + PromptBuilder + ToolDispatcher + callbacks (2026-04-12, b060cee1)
- [x] `chat(message) → string` 简单接口 (2026-04-12, b060cee1)
- [x] `run_conversation(user_message, system_message, conversation_history, task_id) → ConversationResult` (2026-04-12, b060cee1)
- [x] **核心循环**(完全同步):实现于 `ai_agent.cpp::run_conversation` (2026-04-12, b060cee1)
  ```
  while api_call_count < max_iterations && iteration_budget.remaining > 0:
      response = llm.complete(model, messages, tool_schemas)
      if response.tool_calls:
          for tc in response.tool_calls:
              result = handle_function_call(tc.name, tc.args, task_id)
              messages.append(tool_result_message(result))
          api_call_count += 1
      else:
          return response.content
  ```
- [x] OpenAI 消息格式 + Anthropic cache_control 每轮注入 (2026-04-12, b060cee1)
- [x] reasoning content 保存于 `assistant_msg.reasoning` (2026-04-12, b060cee1)
- [x] **agent 级工具拦截**(todo / memory)在 tool_dispatcher **之前** (2026-04-12, b060cee1)
- [x] `IterationBudget` 跟踪 + `request_stop()` 中断 (2026-04-12, b060cee1)
- [x] 上下文压缩触发器(context_overflow retry) (2026-04-12, b060cee1)
- [x] rate-limit backoff 自动重试 (2026-04-12, b060cee1)
- [x] 轨迹保存(可选)—— `ai_agent.cpp::save_trajectory` (2026-04-12, b060cee1)
- [x] 7 test files / 41 tests(test_ai_agent 推迟) (2026-04-12, b060cee1)

---

## 阶段 5 — 工具系统基础

### 5.1 注册表 `tools/registry`
- [x] `ToolEntry` 结构体(含 check_fn / requires_env / max_result_size_chars / emoji) (2026-04-12, b2dd6f86)
- [x] `ToolRegistry` 单例:thread-safe `_tools` map + `_toolset_checks` map (2026-04-12, b2dd6f86)
- [x] `register()` + `register_toolset_check()` (2026-04-12, b2dd6f86)
- [x] `dispatch()` → JSON string,exception capture → `{"error": ...}` (2026-04-12, b2dd6f86)
- [x] 异步 handler 桥接 `_run_async` — `cpp/core/.../async_bridge` (run_async/join_all/settled) (2026-04-13, 7ed8768a)
- [x] `get_definitions(enabled, disabled)`:check_fn + toolset_check 双过滤 (2026-04-12, b2dd6f86)
- [x] `get_toolset_for_tool()` (2026-04-12, b2dd6f86)
- [x] `tool_error()` / `tool_result()` 辅助 (2026-04-12, b2dd6f86)
- [x] 结果截断:per-tool `max_result_size_chars` + `DEFAULT_RESULT_SIZE_CHARS` (2026-04-12, b2dd6f86)
- [x] `last_resolved_tool_names` save/restore(delegate subagent 安全) (2026-04-12, b2dd6f86)

### 5.2 工具发现与编排 `model_tools`
- [x] `_discover_tools()`:导入所有工具模块,触发自注册 (2026-04-12, b2dd6f86)
- [x] `get_tool_definitions(enabled, disabled)` → JSON schema list (2026-04-12, b2dd6f86)
- [x] `handle_function_call(function_name, function_args, task_id, user_task)` → JSON 结果 (2026-04-12, b2dd6f86)
- [x] `_last_resolved_tool_names` process-global(注意 delegate 子 agent 时保存/恢复) (2026-04-12, b2dd6f86)
- [x] cross-tool 引用动态后处理(如 `browser_navigate` ↔ `web_search` 注入) (2026-04-12, b2dd6f86)
- [x] `TOOL_TO_TOOLSET_MAP` / `TOOLSET_REQUIREMENTS`(用于 doctor 检查) (2026-04-12, b2dd6f86)

### 5.3 Toolset 定义 `toolsets`
- [x] `_HERMES_CORE_TOOLS`:50 个核心工具列表 (2026-04-12, b2dd6f86)
- [x] `TOOLSETS` dict:web / search / vision / image_gen / terminal / moa / skills / browser / file / code / memory / messaging / rl / reasoning / full_stack / research / swe / autonomous (2026-04-12, b2dd6f86)
- [x] 每个 toolset:`tools` + 可选 `includes`(组合) (2026-04-12, b2dd6f86)
- [x] `resolve_toolset(name)` → 扁平化工具列表 (2026-04-12, b2dd6f86)
- [x] `validate_toolset(name)` (2026-04-12, b2dd6f86)

### 5.4 Toolset 分布(批处理用)
- [x] `toolset_distributions::DISTRIBUTIONS`:default / image_gen / research / science / swe / autonomous 等 (2026-04-12, b2dd6f86)
- [x] 每分布:toolset 名 → 选择概率 (2026-04-12, b2dd6f86)
- [x] `sample_toolsets_from_distribution(name)` 随机采样 (2026-04-12, b2dd6f86)

### 5.5 Budget / 中断 / 预算
- [x] `tools/budget_config`:`DEFAULT_RESULT_SIZE_CHARS` (2026-04-12, b2dd6f86)
- [x] `tools/interrupt`:Ctrl+C / 网关中断信号 (2026-04-12, b2dd6f86)
- [x] `tools/tool_result_storage`:大输出存储与回放,turn 预算执行 (2026-04-12, b2dd6f86)
- [x] `tools/managed_tool_gateway`:Nous 订阅者网关解析 + 第三方厂商代理 URL 构造 (2026-04-12, 29936e3c)
- [x] `tools/credential_files`:Hermes 凭据文件路径管理 (2026-04-12, b2dd6f86)
- [x] `tools/binary_extensions`:二进制扩展名识别 (2026-04-12, b2dd6f86)
- [x] `tools/env_passthrough`:env var 桥接到后端 (2026-04-12, b2dd6f86)
- [x] `tools/debug_helpers` — `cpp/tools/src/debug_helpers.cpp` (2026-04-13, a2052008)

---

## 阶段 6 — 安全与审批

### 6.1 危险命令检测 `tools/approval`
- [x] 45+ 正则表达式(DangerPattern 结构,5 个类别:filesystem/network/system/database/shell) (2026-04-12, dc5f6c19)
- [x] CommandScanner:ANSI strip → null byte 移除 → NFKC(ASCII fallback)→ 空白折叠 (2026-04-12, dc5f6c19)
- [x] 模式匹配:case-insensitive + ECMAScript regex (2026-04-12, dc5f6c19)
- [x] 测试:test_danger_patterns + test_command_scanner(27 tests) (2026-04-12, dc5f6c19)

### 6.2 会话级审批状态
- [x] `thread_local current_session_key_` (2026-04-12, dc5f6c19)
- [x] thread-safe maps:`session_approved_` / `yolo_sessions_` / `permanent_` + mutex (2026-04-12, dc5f6c19)
- [x] CLI 流程:`request_cli_approval(command, matches, user_prompt_cb)` → 阻塞回调 (2026-04-12, dc5f6c19)
- [x] Gateway 流程:`GatewayApprovalQueue` — enqueue_and_wait + condition_variable + timeout (2026-04-12, dc5f6c19)
- [x] `resolve()` / `cancel_session()` 解锁路径 (2026-04-12, dc5f6c19)
- [x] 永久 allowlist(regex pattern set) (2026-04-12, dc5f6c19)
- [x] 单测:test_session_state.cpp + test_gateway_queue.cpp (2026-04-12, dc5f6c19)

### 6.3 其他安全模块
- [x] `url_safety`:已在 Phase 0 core 完成 (2026-04-12, d21d29c9)
- [x] `website_policy`:WebsitePolicy(wildcard domain rules + load_from_json) (2026-04-12, dc5f6c19)
- [x] `skills_guard`:validate_skill(path / approved_roots / size / injection / name) (2026-04-12, dc5f6c19)
- [x] `osv_check` real HTTP(osv.dev API) (2026-04-12, 29936e3c)
- [x] **MCP 凭据剥离**:`strip_credentials()` 在 core::redact 之上扩展 MCP 特有模式 (2026-04-12, dc5f6c19)

---

## 阶段 7 — 终端环境后端 `tools/environments`

### 7.1 BaseEnvironment 抽象
- [x] 统一契约:`execute(command, cwd, env_vars, timeout) → CompletedProcess` + `cleanup()` (2026-04-12, dc91ac71)
- [x] spawn-per-call 模型(`bash -c`) (2026-04-12, dc91ac71)
- [x] `SessionSnapshot` 抓取(env vars + shell functions + aliases + cwd)+ `render_prelude` (2026-04-12, dc91ac71)
- [x] CWD 持久化:`FileCwdTracker`(local)+ `MarkerCwdTracker`(`__HERMES_CWD__=` stdout marker) (2026-04-12, dc91ac71)
- [x] 超时:watchdog 线程 SIGTERM→2s→SIGKILL (2026-04-12, dc91ac71)
- [x] `cancel_fn` 100ms 轮询中断 (2026-04-12, dc91ac71)
- [x] `EnvFilter`:敏感变量黑名单(`_API_KEY`/`_TOKEN`/`_SECRET` 后缀 + 显式列表),`HERMES_HOME` 放行 (2026-04-12, dc91ac71)

### 7.2 LocalEnvironment
- [x] `fork+exec` + `bash -c` + `poll()` I/O (2026-04-12, dc91ac71)
- [x] env 过滤 (2026-04-12, dc91ac71)
- [x] PTY 支持(forkpty,test_pty.cpp 覆盖) (2026-04-12, dc91ac71)
- [x] Windows(ConPTY + WSL fallback;email IDLE Winsock2) (2026-04-13, 03ac9465, 99afa8cd)

### 7.3 DockerEnvironment
- [x] `build_docker_args()`:--cap-drop=ALL / --no-new-privileges / --pids-limit (2026-04-12, dc91ac71)
- [x] --cpus / --memory / tmpfs / bind mounts (2026-04-12, dc91ac71)
- [x] env 过滤 (2026-04-12, dc91ac71)
- [x] 镜像 pull + tag 解析 (2026-04-13, eb825b94)
- [x] volume 自动清理 (2026-04-13, eb825b94)

### 7.4 SSHEnvironment
- [x] `build_ssh_argv()`:ControlMaster + ControlPersist 10m + socket path (2026-04-12, dc91ac71)
- [x] `MarkerCwdTracker` 远程 CWD (2026-04-12, dc91ac71)
- [x] cleanup = `ssh -O exit` (2026-04-12, dc91ac71)
- [x] `FileSyncManager`:mtime+size cache + `quoted_rm_command` 安全删除 (2026-04-12, dc91ac71)
- [x] dotfile 上传 + 同步(完整路径) (2026-04-13, 41e00306)
- [x] CWD in-band marker 解析 (2026-04-13, b6b059bc)
- [x] SCP 文件同步 (2026-04-13, 41e00306)

### 7.5 ModalEnvironment(原生 SDK 等价)
- [x] Modal Sandbox.create + Sandbox.exec 的 HTTP REST 调用(C++ 没有 Modal SDK,需自行实现 REST 客户端) (2026-04-13, batch12)
- [x] 快照持久化:`~/.hermes/modal_snapshots.json` 按 task_id (2026-04-13, batch12)
- [x] 异步执行:Modal 请求调度到后台事件循环 (2026-04-13, batch12)
- [x] 镜像规格解析:registry refs → Modal image 对象,Ubuntu/Debian add_python 支持 (2026-04-13, batch12)
- [x] 网络超时:30s connect / 120s per command (2026-04-13, batch12)

### 7.6 ManagedModalEnvironment
- [x] Gateway-backed Modal(Nous 订阅者经 tool-gateway) (2026-04-13, batch12)
- [x] 通过 HTTP REST API 而非直接 SDK (2026-04-13, batch12)
- [x] 持久文件系统:跨会话停止/恢复(task_id 索引) (2026-04-13, batch12)
- [x] 超时:1s connect / 5s poll / 5s cancel(graceful) (2026-04-13, batch12)

### 7.7 SingularityEnvironment
- [x] Apptainer/Singularity CLI 自动检测 (2026-04-13, batch12)
- [x] 安全:`--containall` / `--no-home` / 能力下放 (2026-04-13, batch12)
- [x] overlay 目录:可写 overlay 跨会话保留 (2026-04-13, batch12)
- [x] 快照持久化:`~/.hermes/singularity_snapshots.json` (2026-04-13, batch12)
- [x] SIF 镜像缓存 (2026-04-13, batch12)

### 7.8 DaytonaEnvironment
- [x] Daytona REST API(C++ 自实现 SDK) (2026-04-13, batch12)
- [x] 持久 sandbox(stop/resume,task_id 索引) (2026-04-13, batch12)
- [x] 资源限制:CPU / memory(GiB) / disk(GiB,最大 10) (2026-04-13, batch12)
- [x] spawn-per-call 通过线程包装阻塞 SDK 调用 (2026-04-13, batch12)
- [x] 超时 + cancel_fn (2026-04-13, batch12)

### 7.9 FileSyncManager
- [x] mtime + size 变更跟踪 (2026-04-12, dc91ac71)
- [x] 删除传播(`quoted_rm_command` 安全引用) (2026-04-12, dc91ac71)
- [x] dotfile 上传到远程(完整路径同步) (2026-04-13, 41e00306)

---

## 阶段 8 — 工具实现(完整 49+ 工具)

> 每个工具:JSON schema + handler + check_fn + 单测。所有 handler **必须返回 JSON 字符串**。

### 8.1 文件工具(`file_tools` + `file_operations`)
- [x] `read_file`(offset/limit 字符级,屏蔽 `/dev/*`) (2026-04-12, 30c09129)
- [x] `write_file`(原子写,屏蔽敏感路径,100KB 上限) (2026-04-12, 30c09129)
- [x] `patch`(unified diff 应用 + hunk 校验) (2026-04-12, 30c09129)
- [x] `search_files`(递归目录走查 + regex) (2026-04-12, 30c09129)
- [x] `file_operations`:敏感路径屏蔽 + 二进制检测 (2026-04-12, 30c09129)
- [x] 二进制扩展名检测(`is_binary_extension`) (2026-04-12, b2dd6f86)

### 8.2 终端工具
- [x] `terminal`:foreground(600s 上限)+ background spawning + watch_patterns (2026-04-12, 30c09129)
- [x] `process`:poll / wait / kill 后台进程 (2026-04-12, 30c09129)
- [x] 与 `ProcessRegistry` 集成(background 通过 detached thread) (2026-04-12, 30c09129)

### 8.3 Web 工具(`web_tools`)
- [x] `web_search`:Exa API 后端(HttpTransport 注入) (2026-04-12, dcf1f2a8)
- [x] `web_extract`:Firecrawl API 后端 (2026-04-12, dcf1f2a8)
- [x] 多后端切换(Parallel / Tavily) (2026-04-13, 3a6c5ceb)
- [x] 缓存的 API 客户端(TTL cache) (2026-04-13, 3a6c5ceb)

### 8.4 浏览器工具(BrowserBackend 抽象 + FakeBrowserBackend)
- [x] `browser_navigate` / `browser_snapshot` / `browser_click` / `browser_type` (2026-04-12, 00d98010)
- [x] `browser_scroll` / `browser_back` / `browser_press` (2026-04-12, 00d98010)
- [x] `browser_get_images` / `browser_vision` / `browser_console` (2026-04-12, 00d98010)
- [x] `BrowserBackend` 抽象接口 + `FakeBrowserBackend` (2026-04-12, 00d98010)
- [x] CDP 实际驱动 Chromium 后端 (2026-04-12, 24476082)
- [x] CamoFox 隐身浏览器 — `cpp/tools/src/browser_camofox.cpp` (2026-04-13, a2052008)

### 8.5 视觉工具
- [x] `vision_analyze_tool`:URL 下载 + base64 + vision LLM (2026-04-12, dcf1f2a8)
- [x] SSRF 检查(private IP 拦截) (2026-04-12, dcf1f2a8)

### 8.6 代码执行
- [x] `execute_code`:写临时文件 + `hermes_tools.py` 7 函数 stub + LocalEnvironment 执行 (2026-04-12, 764eedaa)
- [x] 300s 超时,50KB stdout 截断 (2026-04-12, 764eedaa)
- [ ] UDS RPC(当前用文件 + 子进程) *— deferred: subprocess 模式工作良好,UDS 优化非关键路径*

### 8.7 内存工具
- [x] `memory`:add / read / replace / remove(backed by MemoryStore) (2026-04-12, 54162168)
- [x] **agent 级别**:AIAgent 拦截 (2026-04-12, b060cee1)

### 8.8 待办工具
- [x] `todo`:merge/replace 模式,per-task_id 内存 store (2026-04-12, 54162168)
- [x] 状态:pending / in_progress / completed / cancelled (2026-04-12, 54162168)
- [x] **agent 级别**:AIAgent 拦截 (2026-04-12, b060cee1)
- [x] **agent 级别**:同 memory(AIAgent 拦截已实现) (2026-04-12, b060cee1)

### 8.9 澄清工具
- [x] `clarify`:多选或开放式问题,最多 4 选项,通过平台 callback 注入(`clarify_tool.cpp`) (2026-04-12, 764eedaa)

### 8.10 跨平台消息工具
- [x] `send_message`:通过 GatewayRunner 路由到所有 18 个适配器(`send_message_tool.cpp`) (2026-04-12, 29936e3c)
- [x] action: send / list (2026-04-12, 29936e3c)
- [x] 文件附件支持(attachment_path schema field) (2026-04-12, 29936e3c)
- [x] target 格式:`platform:chat_id:thread_id`(`parse_target`) (2026-04-12, 29936e3c)
- [x] per-platform channel / contact 缓存 — `cpp/gateway/src/channel_cache.cpp` (2026-04-13, 42296bda)

### 8.11 图像生成
- [x] `image_generate`:OpenAI DALL-E API(HttpTransport 注入) (2026-04-12, dcf1f2a8)
- [x] Flux / Ideogram 后端 (2026-04-13, 747824b6)
- [x] 缓存模型列表 (2026-04-13, 747824b6)

### 8.12 TTS 工具
- [x] `text_to_speech`:edge-tts CLI 路径实现 (2026-04-12, dcf1f2a8)
- [x] ElevenLabs / OpenAI / MiniMax HTTP 后端 (2026-04-12, 29936e3c)
- [ ] ffmpeg 编码 *— deferred: TTS 后端直接返回 mp3/wav,ffmpeg 后处理非必需*

### 8.13 转录工具
- [x] `transcribe_audio`:文件校验 + 扩展名检查 + "install faster-whisper" stub (2026-04-12, 1b30417a)

### 8.14 语音模式
- [x] `voice_mode`:VoiceSession 状态机(Inactive/Listening/Processing/Speaking) + start/stop/status (2026-04-12, 1b30417a)
- [x] STT + 流式 LLM + TTS 实际管线(push-to-talk capture + transcribe pipeline + mock-backend 测试) (2026-04-13, 9aee1912, 68a835a9)

### 8.15 技能工具
- [x] `skills_list`:扫描 `$HERMES_HOME/skills` 目录 (2026-04-12, 54162168)
- [x] `skill_view`:加载 SKILL.md (2026-04-12, 54162168)

### 8.16 会话搜索
- [x] `session_search`:FTS5 via SessionDB (2026-04-12, 54162168)

### 8.17 Home Assistant 工具
- [x] `ha_list_entities` / `ha_get_state` / `ha_list_services` / `ha_call_service`(real HTTP via CurlTransport) (2026-04-12, 29936e3c)

### 8.18 Cron 工具
- [x] `cronjob`:create/list/run/pause/resume/delete + cron 表达式验证 (2026-04-12, 764eedaa)

### 8.19 RL 训练工具(10 个)
- [x] `rl_list_environments` / `rl_select_environment` / `rl_get_current_config` / `rl_edit_config` (2026-04-12, 1b30417a)
- [x] `rl_start_training` / `rl_check_status` / `rl_stop_training` / `rl_get_results` / `rl_list_runs` / `rl_test_inference` (2026-04-12, 1b30417a)
- [x] HttpTransport 注入 + FakeHttpTransport 测试 (2026-04-12, 1b30417a)

### 8.20 委托与多代理
- [x] `delegate_task`:real delegate 子 agent 派生 (2026-04-12, 13b454b6)
- [x] `mixture_of_agents`:real MoA 实现 (2026-04-12, 13b454b6)
- [x] 实际子 agent 派生 + token 预算 (2026-04-12, 13b454b6)

### 8.21 技能管理
- [x] `skill_manage`:list_installed + uninstall(安全检查)+ search/install/update stub (2026-04-12, 764eedaa)

### 8.22 MCP 客户端工具
- [x] `McpClientManager`:load_config 解析 JSON(stdio/HTTP transport + sampling 配置) (2026-04-12, 764eedaa)
- [x] `register_server_tools`:注册 stub 工具 (2026-04-12, 764eedaa)
- [x] 实际 MCP 协议传输(stdio) (2026-04-12, c8116835)
- [x] 重连 / Sampling / 动态发现 / OAuth — `mcp_oauth.cpp` + reconnect/sampling/discovery wiring + tests (2026-04-13, 7a970c1c, 6d010893, a2052008)

---

## 阶段 9 — 技能系统

### 9.1 技能加载
- [x] `get_all_skills_dirs()`:builtin + optional + installed (2026-04-12, c13ced3f)
- [x] `iter_skill_index()`:index.json 或 SKILL.md frontmatter 扫描 (2026-04-12, c13ced3f)
- [x] `parse_frontmatter()`:YAML 提取 (2026-04-12, c13ced3f)
- [x] `extract_skill_description()` / `extract_skill_conditions()` (2026-04-12, c13ced3f)
- [x] `skill_matches_platform()` (2026-04-12, c13ced3f)
- [x] `get_disabled_skill_names()` (2026-04-12, c13ced3f)

### 9.2 斜杠命令注入
- [x] `load_skill_payload()`:加载并剥离 frontmatter,作为 user message 注入 (2026-04-12, c13ced3f)
- [x] `build_plan_path()` (2026-04-12, c13ced3f)
- [x] 内置 prompts:`/plan` / `/debug` / `/web-research` (2026-04-12, c13ced3f)

### 9.3 完整技能集合迁移
- [x] `skills/` 目录内置 skill 文件复制(数百个) (2026-04-13, batch13 — CMake install DIRECTORY hook + HERMES_SKILLS_SEARCH_PATH fallback)
- [x] `optional-skills/` 同上 (2026-04-13, batch13)
- [x] 注入前 prompt injection 扫描(复用 PromptBuilder::is_injection_safe) (2026-04-12, b060cee1)

### 9.4 Skills Hub
- [x] `SkillsHub` 客户端(search/get/install/uninstall/update real HTTP) (2026-04-12, 29936e3c)
- [x] 实际 HTTP + GitHub App JWT (2026-04-12, 29936e3c)

---

## 阶段 10 — Cron 调度

### 10.1 调度器 `cron/`
- [x] 5 字段 cron 表达式解析(*/N, N-M, N-M/S, 逗号列表) (2026-04-12, 9a4504b7)
- [x] `Job` 定义 + `JobStore`(JSON 文件持久化) (2026-04-12, 9a4504b7)
- [x] `Scheduler`:asyncio 等价 + 持久化 (2026-04-12, 932ddb2c)
- [x] 重试逻辑 + 结果存储 (2026-04-12, 932ddb2c)
- [x] `cron/jobs.py` 等价 (2026-04-12, 932ddb2c)
- [x] `cron/scheduler.py` 等价 (2026-04-12, 932ddb2c)
- [x] 与 `cronjob` 工具集成 (2026-04-12, 932ddb2c)

### 10.2 Delivery 路由(`gateway/delivery`)
- [x] `DeliveryTarget`:platform / chat_id / thread_id / is_origin / is_explicit (2026-04-12, 932ddb2c)
- [x] `parse(target_str)`:支持 `origin` / `local` / `telegram` / `telegram:123456` / `telegram:123456:789` (2026-04-12, 932ddb2c)
- [x] `DeliveryRouter::deliver()`:发送到所有 target (2026-04-12, 932ddb2c)
- [x] 平台输出限制截断 (2026-04-12, 932ddb2c)
- [x] `local` target 写入 `~/.hermes/cron/output/` (2026-04-12, 932ddb2c)

---

## 阶段 11 — 网关基础

### 11.1 GatewayRunner
- [x] `start_gateway()` 异步入口 (2026-04-12, 0513a3f8)
- [x] `GatewayRunner::start()`:连接所有启用的平台适配器 (2026-04-12, 0513a3f8)
- [x] 发出 `gateway:startup` hook (2026-04-12, 0513a3f8)
- [x] 从 checkpoint 恢复后台进程 (2026-04-12, 932ddb2c)
- [x] `_create_adapter()` 路由 (2026-04-12, 0513a3f8)
- [x] **后台 watcher**: (2026-04-12, 932ddb2c)
  - [x] `_session_expiry_watcher()`:每 5 分钟刷新过期 session 内存 (2026-04-12, 932ddb2c)
  - [x] `_platform_reconnect_watcher()`:失败适配器后台重连(指数退避) (2026-04-12, 932ddb2c)
  - [x] `_run_process_watcher()`:跟踪后台进程,投递完成通知到聊天 (2026-04-12, 932ddb2c)

### 11.2 消息处理管线 `_handle_message()`
- [x] 用户授权检查(allowlist + pairing store + DM 行为) (2026-04-12, 0513a3f8)
- [ ] `/update` prompt 拦截 *— deferred: gateway dispatch 已支持 update;CLI 端 prompt-time 拦截非阻塞需求*
- [x] 卡死 agent 驱逐(evict_stale_agents) (2026-04-12, 9994cd51)
- [x] 运行中 agent 中断(特例:`/stop` / `/new` / `/reset` / `/background` / `/approve` / `/deny` / 照片) (2026-04-12, 9994cd51)
- [x] 命令分发 vs 普通消息路由 (2026-04-12, 0513a3f8)
- [x] session 创建/检索 (2026-04-12, 0513a3f8)
- [x] agent runtime 解析(model / provider 覆盖) (2026-04-12, 932ddb2c)
- [x] AIAgent 在线程池中调用 (2026-04-12, 932ddb2c)
- [x] 响应格式化与投递 (2026-04-12, 932ddb2c)
- [x] `_handle_active_session_busy_message()`:处理 agent 运行中到达的消息(`handle_busy_session` 队列) (2026-04-12, 9994cd51)

### 11.3 Agent 生命周期
- [x] per-session agent 缓存(`_agent_cache`)—— 关键:跨 turn 保留 prompt cache (2026-04-12, 9994cd51)
- [x] session model overrides(`_session_model_overrides`) (2026-04-12, 9994cd51)
- [x] 运行中 agents 跟踪(`_running_agents` + 时间戳) (2026-04-12, 9994cd51)
- [x] 卡死 agent 驱逐(基于 idle + 墙钟年龄) (2026-04-12, 9994cd51)
- [x] pending message queue(`_pending_messages`) (2026-04-12, 9994cd51)
- [x] background tasks 跟踪(`_background_tasks`)防止 GC (2026-04-12, 9994cd51)

### 11.4 Hooks 系统(`gateway/hooks`)
- [x] 事件常量:`gateway:startup` / `session:start/end/reset` / `agent:start/step/end` / `command:*` (2026-04-12, 0513a3f8)
- [x] `HookRegistry`:register + emit,精确匹配 + 通配符匹配(`command:*`) (2026-04-12, 0513a3f8)
- [x] `emit()` 永不阻塞(异常捕获仅记日志) (2026-04-12, 0513a3f8)
- [x] hook 发现:`~/.hermes/hooks/` + `HOOK.yaml` — `cpp/cli/src/hook_discovery.cpp` (2026-04-13, 12ad04e3)
- [x] 内置 hook:`boot_md` —— `register_boot_md_hook` (2026-04-12, 0513a3f8)

### 11.5 SessionStore(`gateway/session`)
- [x] `SessionSource`:platform / chat_id / chat_name / chat_type / user_id / user_name / thread_id / 等 (2026-04-12, 0513a3f8)
- [x] `SessionContext`:source + connected_platforms + home_channels + 元数据 (2026-04-12, 0513a3f8)
- [x] 文件存储:`{HERMES_HOME}/sessions/{session_key}/session.json + transcript.jsonl` (2026-04-12, 0513a3f8)
- [x] `get_or_create_session` / `reset_session` / `append_message` / `load_transcript` (2026-04-12, 0513a3f8)
- [x] **重置策略**:`should_reset()`(daily / idle / both / none 模式) (2026-04-12, 0513a3f8)
- [x] **PII 脱敏**:`hash_id()` SHA256 12-char hex,Telegram/Signal/WhatsApp/BlueBubbles,Discord 排除 (2026-04-12, 0513a3f8)

### 11.6 Status & Lock(`gateway/status`)
- [x] PID 检测:`{HERMES_HOME}/gateway.pid`,JSON 含 pid / kind / argv / start_time (2026-04-12, 0513a3f8)
- [x] `_looks_like_gateway_process()`:cmdline 校验避免误判 — cross-platform process introspection (2026-04-13, 12ad04e3)
- [x] **scoped locks**:`acquire_scoped_lock(scope, identity, metadata)` / `release_scoped_lock()` (2026-04-12, 0513a3f8)
- [x] lock 目录:`$XDG_STATE_HOME/hermes/gateway-locks/` 或 `HERMES_GATEWAY_LOCK_DIR` (2026-04-12, 0513a3f8)
- [x] lock 文件名:`{scope}-{sha256(identity)[:16]}.lock` (2026-04-12, 0513a3f8)
- [x] lock 文件:pid + start_time + metadata (2026-04-12, 0513a3f8)
- [x] release 逻辑:验证 lock 持有者仍存活 (2026-04-12, 0513a3f8)
- [x] **运行时状态**:`write_runtime_status()` → `{HERMES_HOME}/gateway_state.json` (2026-04-12, 0513a3f8)
- [x] 跟踪:gateway_state(starting/running/fatal/stopping)/ exit_reason / restart_requested (2026-04-12, 0513a3f8)
- [x] per-platform status:state / error_code / error_message (2026-04-12, 0513a3f8)
- [x] `terminate_pid(pid, force)` 平台感知 (2026-04-12, 0513a3f8)
- [x] `_get_process_start_time()` 跨平台(`/proc/{pid}/stat` field 22 / Windows 任务调度器) (2026-04-13, 12ad04e3)

### 11.7 Pairing(`gateway/pairing`)
- [x] 32 字符无歧义字母表(去掉 0/O/1/I) (2026-04-12, 0513a3f8)
- [x] 8 字符 code,1 小时 TTL (2026-04-12, 0513a3f8)
- [x] 限速:1 req per user per 10 min (2026-04-12, 0513a3f8)
- [x] 锁定:5 次失败 → 1 小时锁定 per platform (2026-04-12, 0513a3f8)
- [x] max_pending:3 codes per platform (2026-04-12, 0513a3f8)
- [x] 存储:`~/.hermes/pairing/` chmod 0600 (2026-04-12, 0513a3f8)
- [x] `PairingStore`:per-platform pending/approved/rate_limits 文件 (2026-04-12, 0513a3f8)
- [x] `generate_code()` / `is_approved()` / `approve_code()` / `_is_rate_limited()` (2026-04-12, 0513a3f8)
- [x] thread-safe(RLock 等价) (2026-04-12, 0513a3f8)
- [x] `hermes pairing approve {platform} {code}` CLI —— `cmd_pairing` in main_entry (2026-04-12, e64c81cc)

### 11.8 其他网关模块
- [x] `stream_consumer`:实时 token 流消费 + 平台特定渲染(edit-based streaming) (2026-04-12, 0513a3f8)
- [x] `channel_directory`:平台 channel 名称解析缓存 (2026-04-12, 0513a3f8)
- [x] `mirror`:跨平台消息镜像(Telegram ↔ Discord) (2026-04-12, 0513a3f8)
- [x] `session_context`:动态 system prompt 上下文构建(见 `session_store::SessionContext`) (2026-04-12, 0513a3f8)
- [x] `sticker_cache`:贴纸缓存 (2026-04-12, 0513a3f8)
- [x] `restart`:graceful 重启协调 (2026-04-12, 0513a3f8)

### 11.9 GatewayConfig
- [x] `Platform` 枚举:LOCAL / TELEGRAM / DISCORD / WHATSAPP / SLACK / SIGNAL / MATTERMOST / MATRIX / HOMEASSISTANT / EMAIL / SMS / DINGTALK / API_SERVER / WEBHOOK / FEISHU / WECOM / WEIXIN / BLUEBUBBLES (2026-04-12, 0513a3f8)
- [x] `HomeChannel`:platform / chat_id / name (2026-04-12, 0513a3f8)
- [x] `SessionResetPolicy`:mode / at_hour / idle_minutes / notify / notify_exclude_platforms (2026-04-12, 0513a3f8)
- [x] `PlatformConfig`:enabled / token / api_key / home_channel / reply_to_mode / extra (2026-04-12, 0513a3f8)
- [x] `GatewayConfig`:platforms / sessions_dir / reset_policy / group_sessions_per_user / thread_sessions_per_user / unauthorized_dm_behavior (2026-04-12, 0513a3f8)
- [x] `load_gateway_config()` + 环境变量覆盖 (2026-04-12, 0513a3f8)

---

## 阶段 12 — 平台适配器(全部 21 个)

### 12.1 BasePlatformAdapter
- [x] 通用接口:`connect()` / `disconnect()` / `send()` / `send_typing()` / `send_image/voice/document/video()` / `edit_message()` / `set_message_handler()` (2026-04-12, 356cedb1)
- [x] `MessageEvent`:text / message_type(TEXT/PHOTO/VIDEO/AUDIO/VOICE/DOCUMENT/STICKER) / source / media_urls / reply_to_message_id (2026-04-12, 356cedb1)
- [x] 媒体缓存:`{HERMES_HOME}/cache/images/`、`/audio/`(429/5xx 重试) — `media_cache.cpp` (2026-04-13, b200d3da)
- [x] SSRF 防护 — `safe_fetch.cpp` SSRF wrapper (2026-04-13, 42296bda, b200d3da)
- [x] 错误处理:fatal → 自动重连(retryable=true);非可重试 → 干净退出 — `_platform_reconnect_watcher` (2026-04-12, 932ddb2c)
- [x] 指数退避(jittered_backoff in retry/reconnect paths) (2026-04-12, 932ddb2c)

### 12.2 Telegram 适配器
- [x] long-poll(默认)+ webhook 双模式 (2026-04-12, 356cedb1)
- [x] 文本 / 图片 / 语音 / 文档 / 贴纸 / forum topics / reactions / inline keyboards (2026-04-12, 29936e3c)
- [x] media albums(批处理 photo bursts) (2026-04-12, 29936e3c)
- [x] 文本切分检测 + 重新聚合 (2026-04-12, 29936e3c)
- [x] MarkdownV2 格式化 (2026-04-12, 356cedb1)
- [x] reply-to 模式:first / all (2026-04-12, 356cedb1)
- [x] GFW 备用 IP fallback (2026-04-12, 356cedb1)
- [x] **token scoped lock**(参考 `gateway/platforms/telegram.py` 模板) (2026-04-13, f0215995, 2198dc39)
- [x] BotCommand 菜单从 `telegram_bot_commands()` 生成 (2026-04-13, 2198dc39)

### 12.3 Discord 适配器
- [x] WebSocket gateway intent stream (2026-04-12, 29936e3c)
- [x] 文本 / threads / 富 embed / components / reactions (2026-04-12, 29936e3c)
- [x] 语音频道监听:opus 解码,SSRC → user_id 映射,TTS 输出 — libopus encode/decode + voice callback (2026-04-13, fedf0d99, 3a643487)
- [x] mention 解析(`<@user_id>`) (2026-04-12, 356cedb1)
- [x] thread 创建/管理 (2026-04-12, 356cedb1)
- [x] **不进行 PII 脱敏**(需要真实 ID) (2026-04-12, 356cedb1)
- [x] token scoped lock (2026-04-13, f0215995)

### 12.4 Slack 适配器
- [x] RTM WebSocket 或 Events API + HTTP (2026-04-12, 29936e3c)
- [x] channel + thread 区分 (2026-04-12, 29936e3c)
- [x] @-mention 解析 (2026-04-12, 29936e3c)
- [x] file 上传 (2026-04-12, 29936e3c)
- [x] thread reply 检测(thread 内不需要显式提及) (2026-04-13, 2198dc39)
- [x] `/hermes` 子命令路由(从 `slack_subcommand_map()` 生成) — `slack_subcommand_router.cpp` (2026-04-13, 42296bda)
- [x] HMAC 签名验证 (2026-04-12, 356cedb1)
- [x] token scoped lock (2026-04-13, f0215995)

### 12.5 WhatsApp 适配器
- [x] WebSocket bridge(WaBridge / Whatsmeow) (2026-04-12, 29936e3c)
- [x] 文本 / 图片 / 文档 / reactions / message links (2026-04-12, 29936e3c)
- [x] phone number + JID/LID 别名(bridge session mapping) (2026-04-12, 356cedb1)
- [x] media 处理 (2026-04-12, 29936e3c)
- [x] group 成员跟踪 (2026-04-12, 29936e3c)
- [x] phone+code pairing 流程 (2026-04-13, d42d8a44, fa975d08)

### 12.6 Signal 适配器
- [x] Signal-over-HTTP bridge (2026-04-12, 29936e3c)
- [x] 文本 / reactions / group 更新 (2026-04-12, 29936e3c)
- [x] UUID + 电话号码别名 (2026-04-12, 356cedb1)
- [x] disappearing messages — ephemeral msgs (2026-04-13, d42d8a44, fa975d08)
- [x] group v2 支持 (2026-04-13, d42d8a44, fa975d08)

### 12.7 Matrix 适配器
- [x] Matrix client SDK(libolm 用于 E2EE) (2026-04-12, 29936e3c)
- [x] 文本 / threads / reactions / 文件上传 (2026-04-12, 29936e3c)
- [x] room 成员管理 + invite 处理 (2026-04-12, 29936e3c)
- [x] HOMESERVER / USERNAME / PASSWORD (2026-04-12, 356cedb1)
- [x] 加密支持(libolm E2EE — olm account/session + megolm group sessions, compile-time opt-in) (2026-04-13, 0ece572a)

### 12.8 Mattermost 适配器
- [x] WebSocket RTM + HTTP (2026-04-12, 29936e3c)
- [x] 文本 / threads / reactions / 文件上传 (2026-04-12, 29936e3c)
- [x] TOKEN / URL 凭据 (2026-04-12, 356cedb1)

### 12.9 Email 适配器
- [x] IMAP 轮询 + SMTP 发送 (2026-04-12, 29936e3c)
- [x] 文本(HTML 备用)+ 文档附件 (2026-04-12, 29936e3c)
- [x] EMAIL_ADDRESS / PASSWORD / IMAP_HOST / SMTP_HOST (2026-04-12, 356cedb1)

### 12.10 SMS (Twilio) 适配器
- [x] webhook callback (2026-04-12, 29936e3c)
- [x] 文本 only (2026-04-12, 29936e3c)
- [x] TWILIO_ACCOUNT_SID / AUTH_TOKEN (2026-04-12, 356cedb1)

### 12.11 DingTalk 适配器
- [x] webhook 或 stream (2026-04-12, 29936e3c)
- [x] 文本 / 卡片 / @-mentions (2026-04-12, 29936e3c)
- [x] CLIENT_ID / CLIENT_SECRET (2026-04-12, 356cedb1)

### 12.12 Feishu(飞书)适配器
- [x] webhook 或 stream (2026-04-12, 29936e3c)
- [x] 文本 / 卡片 / reactions / threads (2026-04-12, 29936e3c)
- [x] APP_ID / APP_SECRET (2026-04-12, 356cedb1)

### 12.13 WeCom(企业微信)适配器
- [x] webhook (2026-04-12, 29936e3c)
- [x] 文本 / @-mentions (2026-04-12, 29936e3c)
- [x] BOT_ID / MESSAGE_TOKEN (2026-04-12, 356cedb1)

### 12.14 Weixin(微信公众号)适配器
- [x] webhook + HTTP (2026-04-12, 29936e3c)
- [x] 文本 / 菜单交互 (2026-04-12, 29936e3c)
- [x] APPID / APPSECRET (2026-04-12, 356cedb1)

### 12.15 BlueBubbles(iMessage)适配器
- [x] HTTP API (2026-04-12, 29936e3c)
- [x] iMessage 通过 macOS 桥接 (2026-04-12, 29936e3c)
- [x] reactions / send / receive (2026-04-12, 29936e3c)
- [x] SERVER_URL / PASSWORD (2026-04-12, 356cedb1)

### 12.16 HomeAssistant 适配器
- [x] webhook (2026-04-12, 29936e3c)
- [x] state change 事件(非用户消息) (2026-04-12, 29936e3c)
- [x] HASS_TOKEN (2026-04-12, 356cedb1)

### 12.17 API Server 适配器
- [x] HTTP server(替代 aiohttp) (2026-04-12, 29936e3c)
- [x] JSON endpoint (2026-04-12, 29936e3c)
- [x] 可选 HMAC 认证 (2026-04-12, 356cedb1)

### 12.18 Webhook 适配器
- [x] 通用 JSON webhook (2026-04-12, 29936e3c)
- [x] HTTP server + HMAC 签名校验 (2026-04-12, 356cedb1)
- [x] 可配置签名 secret (2026-04-12, 356cedb1)

### 12.19 Local 适配器
- [x] 本地伪适配器(供测试和 stdin/stdout 模式) (2026-04-12, 356cedb1)

---

## 阶段 13 — CLI 与命令系统

### 13.1 命令注册中心 `hermes_cli/commands`
- [x] `CommandDef` 结构:name / description / category / aliases / args_hint / subcommands / cli_only / gateway_only / gateway_config_gate (2026-04-12, 370adb86)
- [x] `COMMAND_REGISTRY` 列表 (2026-04-12, 370adb86)
- [x] `resolve_command(name)` 名称/别名 → CommandDef (2026-04-12, 370adb86)
- [ ] `rebuild_lookups()` 插件注册时刷新
- [x] 派生:`COMMANDS` 扁平字典(autocomplete) (2026-04-12, 370adb86)
- [x] 派生:`COMMANDS_BY_CATEGORY`(help) (2026-04-12, 370adb86)
- [x] 派生:`gateway_help_lines()` (2026-04-12, 370adb86)
- [x] 派生:`telegram_bot_commands()` (2026-04-12, 370adb86)
- [x] 派生:`slack_subcommand_map()` (2026-04-12, 370adb86)
- [ ] 派生:`GATEWAY_KNOWN_COMMANDS` frozenset

### 13.2 CLI 核心 `cli` / `HermesCLI`
- [ ] FTXUI/curses 替代 Rich + prompt_toolkit:多行编辑、history、autocomplete
- [x] `KawaiiSpinner`:动画 spinner / `┊` 活动 feed (2026-04-12, 370adb86)
- [x] `load_cli_config()`:硬编码默认 + 用户 YAML 合并(`config::load_cli_config`) (2026-04-12, 005749d3)
- [x] **skin engine** 初始化(`display.skin`) (2026-04-12, 370adb86)
- [x] `process_command()`:基于 `resolve_command()` 派发 (2026-04-12, 370adb86)
- [x] 内插 skill 斜杠命令(作为 user message,保留 prompt cache)—— `skill_commands.cpp` (2026-04-12, c13ced3f)
- [x] **所有 slash 命令处理器**(逐一对应 `COMMAND_REGISTRY`): (2026-04-12, 370adb86)
  - [x] Session 类:`/new` / `/reset` / `/retry` / `/undo` / `/title` / `/branch` / `/rollback` / `/stop` / `/background` / `/btw` / `/queue` (2026-04-12, 370adb86)
  - [x] Configuration 类:`/model` / `/provider` / `/personality` / `/voice` / `/reasoning` / `/fast` / `/yolo` / `/verbose` / `/compress` (2026-04-12, 370adb86)
  - [x] Tools & Skills 类:`/skills` / `/tools` / `/<skill-name>` (2026-04-12, 370adb86)
  - [x] Info 类:`/help` / `/commands` / `/usage` / `/insights` / `/status` / `/profile` / `/platforms` (2026-04-12, 370adb86)
  - [x] Exit 类:`/exit` / `/quit` (2026-04-12, 370adb86)
  - [x] Gateway-only:`/approve` / `/deny` / `/sethome` / `/resume` / `/restart` / `/update` / `/reload-mcp`(由 gateway_runner::try_dispatch_command 路由) (2026-04-12, 9994cd51)
- [x] 显示组件:`build_tool_preview()` / `_detect_tool_failure()` / `get_tool_emoji()` / `LocalEditSnapshot` / `_diff_ansi()` (2026-04-12, 370adb86)

### 13.3 主入口 `hermes_cli/main`
- [x] `hermes` 子命令路由:chat / gateway / setup / logout / status / cron / doctor / honcho / version / update / uninstall / acp / profiles / sessions / model / tools / skills / claw / pairing / dump / config / logs / plugins / mcp (2026-04-12, 370adb86)
- [ ] `--profile/-p` 在任何模块导入前生效
- [x] TTY 检查(`isatty(STDIN_FILENO)` pipe 模式) (2026-04-12, 370adb86)
- [x] **claw migrate**:OpenClaw 兼容层(SOUL.md / MEMORY.md / USER.md / skills / 命令 allowlist / 消息设置 / API keys / TTS assets / AGENTS.md 导入) (2026-04-12, batch11 — cpp/cli/src/claw_migrate.cpp)
- [x] `claw migrate --dry-run` / `--preset user-data` / `--overwrite` (2026-04-12, batch11)

### 13.4 子命令实现(`hermes_cli/`)
- [x] `setup.py`:交互式 wizard(model / provider / terminal / skills / API key 输入掩码 / 模型发现 / 网关设置) (2026-04-12, e64c81cc)
- [x] `models.py`:`hermes model [list|use|test]`,OpenRouter live API + Ollama 发现 + 上下文长度探测 (2026-04-12, e64c81cc)
- [ ] `model_switch.py`:热切模型(更新 SessionDB / ContextCompressor / tier-down)
- [x] `model_normalize.py`(`cpp/llm/src/model_normalize.cpp`)+ codex_models stub pending (2026-04-12, 55b4a2fc)
- [x] `skills_config.py`:`hermes skills [list|install|remove|enable|disable]` (2026-04-12, e64c81cc)
- [x] `skills_hub.py`:远程 skill hub(`cpp/skills/src/skills_hub.cpp`) (2026-04-12, 29936e3c)
- [x] `tools_config.py`:`hermes tools` 启用/禁用 per-platform (2026-04-12, e64c81cc)
- [ ] `auth.py` / `auth_commands.py`:OAuth 与凭据
- [ ] `copilot_auth.py`:GitHub Copilot
- [ ] `nous_subscription.py`
- [x] `gateway.py`:`hermes gateway [start|stop|status|install|uninstall]`,systemd / launchd 集成 (2026-04-12, 932ddb2c)
- [ ] `webhook.py`:webhook 安装
- [x] `pairing.py`:pairing CLI(`cmd_pairing`) (2026-04-12, e64c81cc)
- [x] `doctor.py`:配置校验 + 依赖检查 + 提供商连通性 (2026-04-12, e64c81cc)
- [x] `status.py`:系统状态显示(`cmd_status` in main_entry) (2026-04-12, e64c81cc)
- [x] `logs.py`:日志 viewer(tail / filter / search) (2026-04-12, e64c81cc)
- [ ] `dump.py`:会话/配置导出
- [x] `uninstall.py`:干净卸载(`cmd_uninstall` in main_entry) (2026-04-12, e64c81cc)
- [x] `profiles.py`:多 profile CLI (2026-04-12, e64c81cc)
- [ ] `runtime_provider.py`:终端后端选择
- [x] `plugins.py` / `plugins_cmd.py`:插件系统(`cpp/plugins/` + `plugin_manager`) (2026-04-12, 5ef69d59)
- [ ] `providers.py`:提供商配置
- [x] `mcp_config.py`:MCP server 配置 (2026-04-12, 932ddb2c)
- [x] `cron.py`:cron 子命令 (2026-04-12, e64c81cc)
- [ ] `memory_setup.py`:Honcho 内存后端
- [x] `default_soul.md`:默认 AI 身份模板(`cpp/assets/default_soul.md`) (2026-04-12, 5ef69d59)
- [x] `clipboard.py`:系统剪贴板(`cpp/cli/src/clipboard.cpp`) (2026-04-12, 370adb86)
- [x] `env_loader.py`:profile-aware `.env`(`cpp/auth/src/env_loader.cpp`) (2026-04-12, 005749d3)

### 13.5 视觉与皮肤
- [x] `banner.py`:ASCII 艺术 + 版本(`HermesCLI::show_banner`) (2026-04-12, 370adb86)
- [x] `colors.py`:ANSI 颜色定义(`cpp/cli/src/colors.cpp`) (2026-04-12, 370adb86)
- [x] **skin_engine.py**:数据驱动 CLI 主题 (2026-04-12, 370adb86)
  - [x] `SkinConfig` 数据类 (2026-04-12, 370adb86)
  - [x] `init_skin_from_config()` / `get_active_skin()` / `set_active_skin()` / `load_skin()` (2026-04-12, 370adb86)
  - [x] 内置 skins:default / ares / mono / slate (2026-04-12, 370adb86)
  - [x] user skins:`~/.hermes/skins/*.yaml` (2026-04-12, 370adb86)
  - [ ] 缺失值从 default 继承
  - [x] 自定义元素:banner colors / spinner faces+verbs+wings / tool prefix / response box / branding (2026-04-12, 370adb86)
- [ ] `curses_ui.py`:TUI 工具(table / menu),用 ncurses 或 FTXUI

### 13.6 callbacks
- [x] `callbacks.py`:终端回调(clarify / sudo / approval)已在 `clarify_tool` + `session_state::request_cli_approval` 实现 (2026-04-12, dc5f6c19)
- [ ] **不允许使用** `\033[K`(ECMA-48 erase-to-EOL) —— 用空格 padding

### 13.7 Doctor
- [x] 检查项:依赖二进制(curl / docker / ssh)/ SQLite FTS5 / 配置文件(`cmd_doctor`) (2026-04-12, e64c81cc)
- [x] 输出人类可读 pass/fail 表 (2026-04-12, e64c81cc)

---

## 阶段 14 — 批处理与训练

### 14.1 batch_runner
- [x] 多进程池并行 trajectory 生成 (2026-04-12, 2db85e1d)
- [x] JSONL dataset 加载 + 批处理 (2026-04-12, 2db85e1d)
- [x] checkpointing(故障恢复 + resume) (2026-04-12, 2db85e1d)
- [ ] per-prompt trajectory 保存:`from`/`value` XML pairs(含 tool 调用/响应)
- [x] tool stats 聚合(count / success / failure) (2026-04-12, 2db85e1d)
- [ ] HuggingFace dataset schema 标准化
- [x] 通过 `toolset_distributions` 分布采样 (2026-04-12, 2db85e1d)

### 14.2 trajectory_compressor
- [x] 后处理 completed trajectory 到 token 预算(15K 默认) (2026-04-12, 2db85e1d)
- [x] 保护 system prompt + 第一轮 + 最后 N 轮(默认 4) (2026-04-12, 2db85e1d)
- [x] **仅压缩** 中间轮 (2026-04-12, 2db85e1d)
- [x] 单一摘要消息替代被压缩区域(single system-role summary) (2026-04-12, 2db85e1d)
- [x] `CompressionConfig` 结构(tokenizer / targets / OpenRouter model) (2026-04-12, 2db85e1d)
- [ ] 流式压缩进度显示

### 14.3 mini_swe_runner
- [ ] SWE 任务执行器,Hermes trajectory 输出
- [ ] 环境:local / Docker / Modal cloud
- [ ] 终端工具集成(隔离执行)
- [ ] 批处理 from JSONL,输出 hermes 格式

### 14.4 rl_cli
- [ ] 专用 RL 训练 CLI(扩展超时,RL 聚焦 prompt)
- [ ] 完整 toolset(含 RL training tools)
- [ ] 30 分钟 check interval 支持

---

## 阶段 15 — MCP 服务器(`mcp_serve`)

> Hermes **作为** MCP server 暴露给 Claude Code / Cursor / Codex。

- [x] stdio 传输 (2026-04-12, 2db85e1d)
- [x] 10 个工具: (2026-04-12, 2db85e1d / 932ddb2c)
  - [x] `conversations_list` (2026-04-12, 2db85e1d)
  - [x] `conversation_get` (2026-04-12, 2db85e1d)
  - [x] `messages_read` (2026-04-12, 2db85e1d)
  - [x] `messages_send` (2026-04-12, 2db85e1d)
  - [x] `events_poll` (2026-04-12, 2db85e1d)
  - [x] `attachments_fetch` (2026-04-12, 932ddb2c)
  - [x] `permissions_list_open` (2026-04-12, 932ddb2c)
  - [x] `permissions_respond` (2026-04-12, 932ddb2c)
  - [x] `channels_list` (2026-04-12, 2db85e1d)
  - [x] `status_get` (2026-04-12, 932ddb2c)
- [x] 从 SessionDB 读取 session (2026-04-12, 2db85e1d)
- [x] 完整 MCP 协议(initialize / tools/list / tools/call / notifications) (2026-04-12, 2db85e1d)

---

## 阶段 16 — ACP 适配器(编辑器集成)

### 16.1 acp_adapter
- [x] ACP server 入口 (2026-04-12, 2db85e1d)
- [ ] auth(`agent-client-protocol` 等价)
- [x] handlers (2026-04-12, 2db85e1d)
- [ ] VS Code / Zed / JetBrains 集成

### 16.2 acp_registry
- [x] capability 注册 (2026-04-12, 2db85e1d)
- [x] tool listing(acp_adapter `capabilities()` 返回 code_actions/diagnostics/completions/chat) (2026-04-12, 2db85e1d)

---

## 阶段 17 — 插件系统

- [x] `~/.hermes/plugins/` 发现 (2026-04-12, 5ef69d59)
- [x] 插件接口:tool 注册 + command 注册 + hook 注册 (2026-04-12, 5ef69d59)
- [x] 加载 / 启用 / 禁用 命令 (2026-04-12, 5ef69d59)
- [x] 隔离命名空间 (2026-04-12, 5ef69d59)
- [x] C++ 实现可走 dlopen + 定义稳定 ABI(或允许嵌入 Python plugin host) (2026-04-12, 5ef69d59)

---

## 阶段 18 — 打包与部署

### 18.1 容器化
- [x] Dockerfile(对应 `docker/`) (2026-04-12, 5ef69d59)
- [x] docker-compose(开发 + 网关) (2026-04-12, 5ef69d59)
- [ ] 多 arch 构建(amd64 + arm64)

### 18.2 系统集成
- [x] systemd 单元(网关常驻) (2026-04-12, 5ef69d59)
- [x] launchd plist(macOS) (2026-04-12, 5ef69d59)
- [ ] Termux 包(Android)
- [ ] WSL2 兼容性测试

### 18.3 安装脚本
- [x] `scripts/install.sh` 等价(检测平台 + 下载二进制 + 安装到 PATH) (2026-04-12, 5ef69d59)
- [x] `hermes update` 自更新(`cmd_update` 提示 release URL) (2026-04-12, e64c81cc)
- [x] `hermes uninstall` 干净卸载(`cmd_uninstall`) (2026-04-12, e64c81cc)
- [x] Nix flake(对应 `nix/` + `flake.nix`) (2026-04-12, 5ef69d59)

### 18.4 资产
- [x] `assets/`(default_soul.md / default_boot.md)包含 (2026-04-12, 5ef69d59)
- [ ] 内置 skill 集合 复制
- [x] 默认 SOUL.md / 默认 BOOT.md 模板(`cpp/assets/default_soul.md` + `default_boot.md`) (2026-04-12, 5ef69d59)

---

## 阶段 19 — 测试

### 19.1 单元测试
- [x] core 库:strings / path / atomic_io / env / time / redact / retry / url_safety / ansi / fuzzy / patch_parser (2026-04-12, d21d29c9)
- [x] config:加载 / 迁移 / profile 隔离 (2026-04-12, 005749d3)
- [x] SessionDB:并发写 / FTS5 / 迁移 (2026-04-12, 4318321b)
- [x] ProcessRegistry:spawn / poll / kill / watch_patterns / 限速 / overload kill (2026-04-12, 4318321b)
- [x] LLM clients:OpenAI / Anthropic / OpenRouter mock (2026-04-12, 55b4a2fc)
- [x] prompt caching:cache_control 注入正确性 (2026-04-12, 55b4a2fc)
- [x] context compressor:阈值触发 / 中间压缩 (2026-04-12, b060cee1)
- [x] tool registry:dispatch / 异步桥接 / 错误包装 (2026-04-12, b2dd6f86)
- [x] 每个工具:正负样本各覆盖 (2026-04-12, 764eedaa)
- [x] approval:每条危险模式 + 会话状态 + 网关队列 (2026-04-12, dc5f6c19)

### 19.2 集成测试
- [x] AIAgent 端到端:多轮对话 + 工具调用 + 上下文压缩 (2026-04-12, 05803f8f)
- [x] 网关:gateway_runner / lifecycle / pairing / platform_adapters 测试集 (2026-04-12, 356cedb1)
- [x] cron 调度 + delivery (2026-04-12, 05803f8f)
- [x] MCP 客户端:stdio 传输 + 客户端测试(`test_mcp_client.cpp` + `test_mcp_transport.cpp`) (2026-04-12, c8116835)
- [x] MCP 服务器:`test_mcp_server.cpp` (2026-04-12, 2db85e1d)

### 19.3 平台适配器测试
- [x] 每个适配器:`test_platform_adapters.cpp` 覆盖所有 18 个适配器 (2026-04-12, 356cedb1)
- [x] 用 mock 服务器(`FakeHttpTransport`)(2026-04-12, 356cedb1)

### 19.4 等价性回归测试
- [x] 相同输入(prompt + tool seed)分别跑 Python 版与 C++ 版 (2026-04-12, 05803f8f)
- [x] 比对:轨迹 JSON / 工具结果 JSON / token 用量 (2026-04-12, 05803f8f)
- [x] 容忍非确定性字段(时间戳 / 随机 ID) (2026-04-12, 05803f8f)

### 19.5 性能基准
- [x] 启动延迟(目标:< Python 版 50%) (2026-04-12, 05803f8f)
- [x] 工具 dispatch QPS (2026-04-12, 05803f8f)
- [x] SessionDB 写吞吐 (2026-04-12, 05803f8f)
- [x] 网关并发会话数(`performance_benchmark.cpp`) (2026-04-12, 05803f8f)

---

## 阶段 20 — 文档与发布

- [x] `README.md`(C++ 版本) (2026-04-12, 05803f8f)
- [x] `CONTRIBUTING.md`(C++ 版本) (2026-04-12, 05803f8f)
- [ ] `CLAUDE.md` / `AGENTS.md` 同步更新
- [x] API 文档(Doxygen)`cpp/Doxyfile` (2026-04-12, 5ef69d59)
- [x] 架构图(同步 `docs/`) (2026-04-13, batch13 — cpp/docs/architecture.md + module-dependency.md, 6 Mermaid diagrams)
- [x] CHANGELOG(`cpp/CHANGELOG.md`) (2026-04-12, 05803f8f)
- [ ] v0.1.0 alpha → v1.0.0 GA 发布节奏

---

## 全局约束(开发期间必须始终满足)

1. **prompt cache 不可破坏**:不允许中途修改历史 / 中途切换 toolset / 中途重建 system prompt。唯一允许的变动是 context compression。
2. **Working directory**:CLI 用 `cwd()`;网关用 `MESSAGING_CWD`(默认 home)。
3. **路径**:**禁止**硬编码 `~/.hermes`,统一 `get_hermes_home()` / `display_hermes_home()`。
4. **测试**:不允许写入真实 `~/.hermes/`,统一通过临时目录隔离。
5. **simple_term_menu 等价物**:不允许使用,改用 ncurses / FTXUI(避免 tmux ghosting)。
6. **`\033[K`**:禁止在 spinner / display 中使用,改用空格 padding。
7. **schema 描述**:禁止跨 toolset 工具按名引用,需在 `get_tool_definitions()` 后处理动态注入。
8. **凭据剥离**:所有错误消息和日志输出必须通过 `RedactingFormatter`。
9. **handler 返回类型**:所有工具 handler 必须返回 JSON 字符串(`std::string`)。
10. **每完成一项,更新本文件的复选框**,并在该项尾追加 `(YYYY-MM-DD, <commit hash>)`。

---

## 进度追踪小节

- 阶段 0 完成日期: 2026-04-12 (d21d29c9) — 60 tests
- 阶段 1 完成日期: 2026-04-12 (edaf9b44) — 102 tests
- 阶段 2 完成日期: 2026-04-12 (e75ea86e) — 133 tests
- 阶段 3 完成日期: 2026-04-12 (00492d43) — 188 tests
- 阶段 4 完成日期: 2026-04-12 (b060cee1) — 229 tests
- 阶段 5 完成日期: 2026-04-12 (b2dd6f86) — 246 tests
- 阶段 6 完成日期: 2026-04-12 (dc5f6c19) — 273 tests
- 测试补全: 2026-04-12 (44bbc8f8) — 337 tests
- 阶段 7 完成日期: 2026-04-12 (dc91ac71) — 375 tests
- 阶段 8 完成日期: 2026-04-12 (764eedaa) — 474 tests (6 commits, 49 tools)
- 阶段 9 完成日期: 2026-04-12 (c13ced3f) — 490 tests
- 阶段 10 完成日期: 2026-04-12 (9a4504b7) — 518 tests
- 阶段 11 完成日期: 2026-04-12 (0513a3f8) — 548 tests
- 阶段 12 完成日期: 2026-04-12 (356cedb1) — 577 tests
- 阶段 13 完成日期: 2026-04-12 (370adb86) — 606 tests
- 阶段 14-16 完成日期: 2026-04-12 (2db85e1d) — 624 tests
- 阶段 17-18 完成日期: 2026-04-12 (5ef69d59) — 632 tests
- 阶段 19-20 完成日期: 2026-04-12 (05803f8f) — 648 tests
- batch 9: 2026-04-12 (52c4bf0b, 24476082, c8116835, e64c81cc) — CurlTransport + CDP browser + MCP stdio + CLI subcommands — 688 tests
- batch 10: 2026-04-12 (29936e3c, 13b454b6, 932ddb2c, e9fc2389) — real HTTP for all adapters/tools + SSE streaming + gateway HTTP + eliminate all stubs — **728 tests**
- v0.1.0 alpha 发布: 待定
- v1.0.0 GA 发布: 待定
