# Tokens 参考来源适配

核实日期：2026-10-09。实现为原生 C++，没有调用 Rust、Node、浏览器、登录接口或远端同步程序。

对照 [missuo/tokens README](https://github.com/missuo/tokens) 的 **41 个客户端**，与原来 26 个来源合并后目录共 **56 项**。目录存在表示可选择该来源导入，不等于已经采集成功。只有实际采到用量才出现在统计列表。保留原来的 ChatGPT、Manus、Dify、Coze、国内云平台等入口。

“原生日志”仅包含真正的请求计量和可用日期；“已有缓存”只读取用户磁盘上已经存在的 Tokens 同步结果，本软件不获取 Cookie、API 密钥或触发同步；“需导出”没有足够的真实逐日 Token 证据，接受 `tokenscope.usage.v1` 或支持的实际 API usage 导出，不能据此宣称自动原生采集。

## 41 个客户端逐项对应

Windows 主目录用 `%USERPROFILE%`，编辑器目录用 `%APPDATA%/<编辑器>/User/globalStorage`。`share` 是 `XDG_DATA_HOME` 或 `~/.local/share`。Tokens 缓存优先尊重非空 `TOKENS_CONFIG_DIR`，设置时仅扫描该根；未设置时扫描 Windows 正确的 `%APPDATA%/tokens`，并兼容 `(XDG_CONFIG_HOME 或 ~/.config)/tokens`，相同 Windows 路径去重。TRAE 国内/国际产品与自行生成缓存的区别见 [trae-setup.md](trae-setup.md)。

| 参考客户端 | 本软件来源 ID | 接入方式与主要位置 |
| --- | --- | --- |
| Amp | `amp` | 原生日志：`share/amp/threads/*.json`，只用 usageLedger 的逐请求日期。 |
| Antigravity | `antigravity` | 已有缓存：`tokens/antigravity-cache/sessions/*.jsonl`。 |
| Antigravity CLI | `antigravity-cli` | 原生只读 SQLite：`~/.gemini/antigravity-cli/conversations/*.db`。 |
| Claude Code | `claude` | 原生日志：`~/.claude/projects` 与 `transcripts`。 |
| Cline | `cline` | 原生日志：扩展 `saoudrizwan.claude-dev/tasks`，以及 `~/.cline/data/sessions`。 |
| CodeBuddy | `codebuddy` | 原生日志：`~/.codebuddy/projects`；已验证的真实 usage 字段。 |
| Codebuff | `codebuff` | 原生日志：`~/.config/manicode/projects/*/chats/*/chat-messages.json`。 |
| Codex CLI | `codex` | 原生日志：`~/.codex/sessions` 与 `archived_sessions`。 |
| Command Code | `commandcode` | 原生日志：`~/.commandcode/projects/**/*.jsonl`；仅 v3 明确 usage。 |
| Copilot | `copilot` | 原生日志：`~/.copilot/otel/*.jsonl`；另兼容逐请求 SDK assistant.usage。 |
| Crush | `crush` | 需导出：本地 prompt/completion 是上下文快照；Cost 不能换成 Token。 |
| Cursor | `cursor` | 已有缓存 `tokens/cursor-cache/usage*.json/csv`，以及官方 CSV 导入。 |
| Devin CLI | `devin-cli` | 原生只读 SQLite：`share/devin/cli/sessions.db` 的 message_nodes。 |
| Devin Desktop | `devin-desktop` | 需导出：ACP 累计/缺调用身份的事件不能安全分日或去重。 |
| Droid | `droid` | 需导出：settings.tokenUsage 是会话累计，不按正文长度分摊。 |
| Fx | `fx` | 需导出：usage-v2.json 是会话累计，不全计到更新时间当天。 |
| Gemini CLI | `gemini-cli` | 原生日志：`~/.gemini/tmp/*/chats` JSONL 与旧 JSON。 |
| Goose | `goose` | 需导出：sessions.db 主要是会话累计，不全计到创建当天。 |
| Grok Build | `grok-build` | 原生日志：`~/.grok/logs/unified.jsonl` 中 inference_done。 |
| Hermes Agent | `hermes` | 需导出：session_model_usage 的累计缺少每次调用日期。 |
| Jcode | `jcode` | 原生日志：`~/.jcode/sessions` JSON snapshot / JSONL journal。 |
| Junie | `junie` | 原生日志：`~/.junie/sessions/*/events.jsonl` 的 LlmResponseMetadataEvent。 |
| Kilo | `kilo-code` | 原生日志：历史扩展 `kilocode.kilo-code/tasks`。 |
| Kilo CLI | `kilo-cli` | 原生只读 SQLite：`share/kilo/kilo.db` 的 OpenCode 兼容 message 表。 |
| Kimi | `kimi-code` | 原生日志：新 `~/.kimi-code` 与旧 `~/.kimi` wire。与 Kimi 网页来源独立。 |
| Kiro | `kiro` | 需导出：不按上下文占比和字符数估算 Token。 |
| MiMo Code | `mimo-code` | 原生只读 SQLite：`share/mimocode/*.db` 的 OpenCode 兼容 message 表。 |
| Mux | `mux` | 需导出：byModel 累计与 lastRequest 时间不能还原逐日用量。 |
| OpenClaw | `openclaw` | 原生 JSONL / `openclaw-agent.sqlite` 的 transcript_events。 |
| OpenCode | `opencode` | 原生只读 SQLite message 表 / 旧 storage/message JSON。 |
| OpenCodeReview | `opencodereview` | 原生日志：`~/.opencodereview/sessions` 的 llm_response；缓存口径需可验证。 |
| Orca | `orca` | 需导出：不把共享 Claude/Codex/MiMo hook 副本重复归属 Orca。 |
| Pi | `pi` | 原生日志：`~/.pi/agent/sessions`；兼容同格式 `~/.omp/agent/sessions`。 |
| Qwen | `qwen-code` | 原生日志：`~/.qwen/projects` 与旧 tmp 的 chats JSONL。 |
| Reasonix | `reasonix` | 原生日志：`~/.reasonix/stats/*.jsonl`，不读取无计量的 transcript。 |
| Roo Code | `roo-code` | 原生日志：扩展 `rooveterinaryinc.roo-cline/tasks`。 |
| Trae | `trae-code` | 已有缓存：`tokens/trae-cache/sessions/*.json`；不解密本地数据库。 |
| Warp | `warp` | 需导出：现有缓存只有 requests/spend，没有 Token 分项。 |
| WorkBuddy | `workbuddy` | 原生日志：`~/.workbuddy/projects` 的 providerData.rawUsage；不采用累计库兜底。 |
| ZCode | `zcode` | 原生只读 SQLite：`~/.zcode/cli/db/db.sqlite` 的 model_usage。 |
| Zed Agent | `zed` | 需导出：thread 累计无法确定逐请求日期。 |

原生日志与已有缓存合计 30 个参考客户端；另 11 个明确需要真实用量导出。此数量不是模型白名单或本机已发现软件数。每家软件内按实际日志中的模型 ID 分行，未记录模型时保留未知。

合并后的 56 项按严格能力划分为：28 个具备直接原生日志/SQLite 解析器（其中 CodeBuddy 仅核实共享格式，尚无本机真实会话验证）、3 个读取已有同步缓存（Cursor、TRAE TraeCode、Antigravity），25 个需要实际用量导出。25 个还包括原有的 ChatGPT、TraeWork、Manus、Gemini Deep Research、Kimi 网页、MiniMax Code / Design、Coze、Dify、n8n、百炼、智谱平台、OpenAI Agents SDK/API、Qoder，以及上述 11 个新增需导出的客户端。Qoder 缓存没有计量字段，不能因配置了候选目录就算作原生已接入。所有 56 项都保留规范/API 用量导入。

## 主适配器口径

- TRAE：使用 `session_id`、`usage_time`（Unix 秒）、`model_name`、`extra_info.input_token/output_token/cache_read_token/cache_write_token`；输入与缓存为独立桶。Auto 空模型保留未知，忽略 dollar_float / credits。键含会话、时间和实际模型；同会话同秒同模型且无独立 ID 的更新视为同条累计快照，无法证明是独立调用时不任意相加。
- Cursor：严格读取 `usageEventsDisplay[].tokenUsage` 的实际 Token 分项。其 inputTokens、cacheReadTokens、cacheWriteTokens 为独立桶，统一为含缓存总输入。实际 requestId 与 CSV Request ID 共用命名空间；无 ID 时需要 conversationId、真实毫秒时间、计量签名及同签名序号。Auto/agent_review 不是具体模型，保留未知。自动缓存目录同名 JSON 优先于旧 CSV，忽略归档子目录；用户显式选择 CSV 仍可导入。没有共享 ID 的额外 CSV 和 API 缓存无法安全判断是否同一批请求，应只选择一个渠道；仅 Cost/Credits 的记录不采集。
- Copilot：只接收 OTel chat span、inference details 和 SDK assistant.usage；忽略 invoke_agent、turn/session 汇总和上下文占用。输入/输出采用 OTel 总量，缓存/推理为子集。优先真实 response ID，后备非零 trace/span；不使用行号、文件修改时间或 session 当前模型。chat span 保持固定 trace/span 键，并通过持久化请求 scope 覆盖同身份 inference/SDK，支持跨文件、跨扫描与后来补 response ID。没有共享身份的独立导出无法推断关系。
- Antigravity IDE 缓存：仅 type=usage，input/cacheRead/cacheWrite 独立，output/reasoning 独立；实际 responseId 优先。未知模型不拿 session 当前设置回填。
- Antigravity CLI：采用参考源码已核实的 protobuf 字段，仅 gen_metadata 生成计量；读取真实 per-generation 时间或 steps 的对应 responseId/generation idx 时间。忽略 opaque 八字节推测时间、会话 created_at 和文件 mtime。嵌套 protobuf 损坏整体拒绝；没有逐次时间或 responseId 的记录不采集。此格式是参考项目对实际文件的逆向结构，厂商未发布稳定 protobuf schema，后续格式变化需要重新验证。
- Command Code：仅 v3 assistant message.usage，inputTokens 已含缓存。历史已消费调用即使 rewind 后仍保留；复制/fork 同 entry ID+timestamp 去重。旧版按正文字符数估算的做法不采用。
- Reasonix：prompt、completion 已含 cache_hit/reasoning；明确非零 cache_miss 可恢复输入分桶，剩余总量存未分类。只统计 stats，跳过 turn。无 ID 的格式使用时间、模型、请求次数、计量签名及同身份序号；完整副本稳定，重新排列或裁剪日志无法保证相同身份。
- Jcode：只接收 assistant token_usage 与消息 ID/真实时间。session 当前模型/provider 不回填历史。cache_creation 字段存在或 cache_read>input 时确认分桶输入，否则缓存为 input 子集。reasoning 是输出子集。snapshot/journal 同 message ID 去重。
- Junie：仅 LlmResponseMetadataEvent.modelUsage，使用真实 timestampMs；数组内序号区分同一事件的不同调用。分桶 input/cache 和 output/reasoning 转为总量；没有请求 ID 时需要保留 session 目录名。无逐次时间拒绝，不回退 mtime。
- Grok Build：只接 shell.turn.inference_done 的 prompt_tokens/completion_tokens/cached_prompt_tokens/reasoning_tokens 与真实 ts、sid、request_id 或 loop_index。总输入/输出已有缓存和推理；旧 signals/context 占用不作为消费。未记录的实际模型保留未知。
- Kilo CLI / MiMo：复用已验证的 OpenCode message schema；input 加独立 cache.read/cache.write，reasoning 为 output 子集。所有软件由 Store 的 source ID 隔离，不因消息 ID 相同互相合并。
- Devin CLI：只读 message_nodes.chat_message.metadata.metrics，真实 created_at Unix 秒与 row_id/session_id；generation_model 是每条实际模型，adaptive 置未知；不使用会话当前模型或 session 累计。
- ZCode：读取逐调用 model_usage、started_at/completed_at。现代 computed_total_tokens 验证 inclusive 或明确完全分桶版本；其他可信总量保留未分类，不误拆分计价。老版本缓存口径不能验证时拒绝，不采用文本估算。
- OpenCodeReview：仅 llm_response、实际日期、ID 或 session 范围+请求签名；不使用 mtime。带缓存但缺少总量无法证明 prompt 是否已含缓存时拒绝；明确 total_tokens 验证后才分类。

消息家族和官方写盘依据另见 [extended-families.md](extended-families.md)，原有 CLI 依据见 [cli-adapters.md](cli-adapters.md)。

## 环境、读取边界和验证

遵循现有 CODEX_HOME、CLAUDE_CONFIG_DIR、KIMI_CODE_HOME、KIMI_SHARE_DIR、QWEN_RUNTIME_DIR/QWEN_HOME。新增路径遵循 XDG_DATA_HOME、XDG_CONFIG_HOME、CODEBUFF_DATA_DIR、REASONIX_STATE_HOME/REASONIX_HOME、JCODE_HOME、GROK_HOME。

重新核实 [Gemini 官方 paths.ts](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/utils/paths.ts) 与 [Storage.getGlobalGeminiDir](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/config/storage.ts)：`GEMINI_CLI_HOME` 替代用户 home，仍需拼接 `.gemini/tmp`。参考项目 clients.rs 将其直接当 `.gemini` 根目录的行为未照搬。Antigravity CLI 是独立程序，尚未核实其自定义目录的官方语义，自动路径只采用默认目录，可以手动选择其他目录导入。

只遍历明确应用目录，最多 12 层、50,000 个目录项；跳过 Windows junction/reparse point。JSON 文档最大 64 MiB，JSONL 单条最大 16 MiB，只消费完整换行。SQLite 使用 READONLY、500ms busy timeout 和取消 progress handler，不更改软件数据库。只把用量元数据发给 Store，不保留正文、工具参数或凭据。

`extended_tests.cpp` 验证真实缓存/OTel 格式、跨渠道持久去重、晚到 response ID、严格日期、等时间独立记录、readonly SQLite、protobuf 截断、逐次时间、取消和目录兼容；`extended_family_tests.cpp` 验证消息家族。原 `sources_integration_tests.cpp` 按动态目录遍历全部来源，覆盖规范/API 导入、重复/重启、迁移、metadata-only 与取消。

## 尚未纳入的最新模块

参考 README 的 41 项之外，当前 clients.rs 还出现 Gajae-Code、Senpi、Augment、Kimchi、Prime Agent、Freebuff、Cherry Studio、DSH、MCode、OMP、LM Studio、Unsloth、Hindsight、Craft Agent。DSH 已由 DeepSeek Harness 接入，OMP 仅复用 Pi 格式目录。其余需要分别核实真实文件、身份、日期与缓存口径，不能仅复制名称就声称已采集。

## MIT 归属

参考项目：[missuo/tokens](https://github.com/missuo/tokens)，基于 [junhoyeo/tokscale](https://github.com/junhoyeo/tokscale)。本适配器参考其 sessions 目录的字段、路径和测试样本，改写为 C++，并在统计口径、日期、模型归属和去重上增加保守限制。原 MIT 许可证及版权归属随本软件第三方声明保留。

本轮下载快照：2026-10-09 `main` ZIP，SHA256 `FF2159DA7FB8B283B6075D8357FC22FDC8580BABEFD0A673F8EB8F82FA0F6C84`。上游未执行任何脚本。版本定位以保存的 ZIP 校验值为准，不将可变 main 分支当不可变 commit。
