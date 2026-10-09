# CLI 与 Cursor 用量适配

本轮读取的都是来源提供的实际计量字段。不会把对话字数、上下文占用、积分、Premium Request 或 Credits 转换成 Token。模型名称没有白名单；记录没有真实模型时保留“未知模型”。以下格式均有离线 fixture 测试，但本机未安装所有软件，不能把 fixture 验证等同于逐家实机验证。

| 来源 | 支持的输入 | 自动采集边界 |
|---|---|---|
| OpenCode | `message(id,data)` 的只读 SQLite；旧版 `storage/message/<session>/*.json` | 当前支持 V1 message 计量，未解析未来 V2 session_message 格式。数据库与旧文件按消息身份去重；不读取 session 聚合以免重复。 |
| Gemini CLI | `~/.gemini/tmp/<project>/chats/**/*.jsonl`；旧版完整会话 JSON | 稳定 message ID，兼容迁移与 JSONL 全量 checkpoint。重写/重复记录由 Store 合并，不重复相加。 |
| Qwen Code | `~/.qwen/tmp/<project>/chats/**/*.jsonl` assistant 的 usageMetadata | uuid 是消息身份，分支复制不重复。只读取显式历史 model，不用当前设置推断。新的 Managed Session 事务封装暂未解析。 |
| Kimi Code CLI | 新版 `~/.kimi-code/sessions/**/agents/*/wire.jsonl` 的 usage.record；旧版 `~/.kimi/sessions/**/wire.jsonl` 的 StatusUpdate | 新版读取每次调用用量，主智能体和子智能体均包含；实际请求模型取同一日志先行的 llm.request，不把 usage.record 中的配置别名当成实际模型。旧版没有实际模型时显示未知。 |
| GitHub Copilot | SDK 显式导出的 `assistant.usage` 事件 | 官方 SDK 定义该事件为 ephemeral，常规 events.jsonl 通常没有；发现安装或会话不等于已取得用量。shutdown 的累计统计不分配到结束日期。 |
| Cursor | 官方 Dashboard Usage CSV | 目前是导入渠道，不宣称自动读取 IDE 本地数据库。列必须包含实际计量字段；不从 Credits、代码行或本地聊天数据库大小推算。 |

自动发现遵循来源的自定义目录：OpenCode 使用 `XDG_DATA_HOME`，新版 Kimi 使用 `KIMI_CODE_HOME`，旧版使用 `KIMI_SHARE_DIR`；Qwen 的运行目录优先 `QWEN_RUNTIME_DIR`，其次 `QWEN_HOME`，再回退到 `~/.qwen`；Gemini 的 `GEMINI_CLI_HOME` 替换用户 home，实际目录仍追加 `.gemini/tmp`。软件设置中的其他自定义运行目录可手动选择导入。

## 计量口径

- OpenCode：输入总量 = `tokens.input + tokens.cache.read + tokens.cache.write`；output 已是输出总量，reasoning 作为其中的细项。
- Gemini CLI：输入包含 cached；输出总量 = `tokens.output + tokens.thoughts`。Google API 的 candidates 和 thoughts 分开。工具 prompt 不再次相加。
- Qwen Code：名称虽为 usageMetadata，但其 OpenAI converter 把 completion_tokens 直接保存为 candidatesTokenCount，思考通常已包含在输出中。只有 totalTokenCount 明确等于 input + candidates + thoughts 才补加 thoughts；等于 input + candidates 时不补加。无 total 时按来源的 candidates 输出量保留，避免重复计数。仅有 total 时保留未分类 Token，不虚构输入输出拆分。
- Kimi：新版输入总量 = `inputOther + inputCacheRead + inputCacheCreation`，旧版使用对应 snake_case 字段。新版仅计 usage.record，不重复计 step.end、agent.status.updated 汇总或 token_counting.measured 上下文占用；llm.request.provider 是协议名，不当作供应商身份。来源在供应商未提供 usage 时会补 emptyUsage()，因此全零记录不作为已计量调用入库。
- Copilot：直接保留逐请求 inputTokens/outputTokens；缓存与 reasoning 均是细项。不把 nano-AIU 或 multiplier 当作美元费用。
- Cursor CSV：`Input (w/ Cache Write)` 已包含非缓存输入和 cache write；缓存写入 = 它减去 `Input (w/o Cache Write)`；输入总量 = `Input (w/ Cache Write) + Cache Read`。

## Cursor CSV 导入要求与身份

必需表头是 `Date`、`Model`、`Input (w/ Cache Write)`、`Input (w/o Cache Write)`、`Cache Read`、`Output Tokens`。存在 `Total Tokens` 时会校验输入输出合计。支持 UTF-8 BOM、CRLF、带逗号和转义引号的 CSV 单元格。

Date 必须含明确 `Z` 或时区偏移。缺少时区时不会擅自使用导入当天或本机当前时区解释历史记录。CSV 中 `Cost=included` 不等于零 Token，也不被当作 API 模型费率。

优先使用 `Request ID` 或 `Event ID`。官方导出没有 ID 时，回退身份使用时间、模型、provider、Token 分项的内容摘要，再加同一份导出中完全一致记录的出现序号。这样相同请求在重命名、重复导出后不会重复，同时保留同一毫秒完全相同的多行调用。局限：在没有稳定 ID 的条件下，来自两个互不重叠导出但所有计量字段完全一致的独立调用，无法可靠区分；应优先导入完整范围或含真实请求 ID 的导出。

新版 Kimi 的 usage.record 同样没有稳定请求 ID，使用 wire 创建时间、来源原始毫秒时间、模型别名、计量分项摘要与同一 wire 内出现序号作回退身份。复制或 fork 的 wire 保持去重；两个独立日志的创建时间、事件毫秒时间、别名与用量全部相同时仍可能碰撞。应保留 wire 的 metadata 头；单独截取事件缺少创建时间，不能可靠地与完整日志对应。缺少相邻 llm.request 或别名不匹配时，模型保留未知，不使用当前配置补填。旧版缺少 message_id 时保留原始精度时间回退身份，同一精确时间的独立 step 仍可能碰撞。

## 读取与隐私

SQLite 仅以 `SQLITE_OPEN_READONLY` 打开，设置短暂 busy timeout，不改源库。JSONL 只处理完整换行记录，活跃写入的半行留待重扫；单行限制 16 MiB，完整 JSON/CSV 限制 64 MiB，目录深度和文件数量有边界。用量发射结构仅含请求身份、时间、模型、provider 与计数；对话内容不进入统计数据库。

## 官方依据

- [OpenCode V1 消息与 SQLite schema](https://github.com/anomalyco/opencode/blob/dev/packages/core/src/session/sql.ts)
- [OpenCode V1 计量归一化实现](https://github.com/anomalyco/opencode/blob/v1.2.27/packages/opencode/src/session/index.ts)
- [Gemini CLI 记录类型](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/services/chatRecordingTypes.ts)
- [Gemini CLI 会话记录与迁移](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/services/chatRecordingService.ts)
- [Google UsageMetadata 计量口径](https://ai.google.dev/api/generate-content#UsageMetadata)
- [Qwen Code 会话记录](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/services/chatRecordingService.ts)
- [Qwen Code OpenAI converter](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/core/openaiContentGenerator/converter.ts)
- [Qwen 自定义运行目录](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/config/storage.ts)、[Gemini 自定义 home](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/utils/paths.ts)
- [Kimi wire 文件](https://github.com/MoonshotAI/kimi-cli/blob/main/src/kimi_cli/wire/file.py)、[StatusUpdate](https://github.com/MoonshotAI/kimi-cli/blob/main/src/kimi_cli/wire/types.py)、[TokenUsage](https://github.com/MoonshotAI/kimi-cli/blob/main/packages/kosong/src/kosong/chat_provider/__init__.py)
- [新版 Kimi 会话目录](https://github.com/MoonshotAI/kimi-code/blob/main/docs/en/guides/sessions.md)、[逐次用量](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/session/usage/usageAgentModel.ts)、[实际请求模型及别名](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/agent/llmRequester/llmRequesterService.ts)、[新版 TokenUsage](https://github.com/MoonshotAI/kimi-code/blob/main/packages/agent-core-v2/src/human/llm/usage.ts)
- [Copilot SDK 官方事件 schema](https://github.com/github/copilot-sdk/blob/main/nodejs/src/generated/session-events.ts)
- [Cursor 官方 CSV 入口说明](https://cursor.com/docs/account/teams/analytics)

2026-10-09 补充：Qwen 自动目录增加 `projects`，保留旧 `tmp`。当前 projects 消息可能没有 uuid，必须有真实 sessionId 和 timestamp；完整日志内以同身份出现序号区分同时请求，完整副本/重命名文件可去重，裁剪或改变同时间记录顺序无法保证原身份。GEMINI_CLI_HOME 重新核实为替代用户 home，仍拼接 `.gemini/tmp`。Copilot OTel、Cursor 已有缓存和其余新增客户端的口径见 [extended-adapters.md](extended-adapters.md)。

核实日期：2026-10-08/09。上游格式后续仍可能变化；无法匹配时应显示没有可计量记录，而不是推测数量。
