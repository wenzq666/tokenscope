# 国产来源适配核实记录

本轮实现 `src/adapters/domestic.cpp`。统计只取来源已经写出的计量字段，不根据正文长度、积分、当前模型设置推测历史用量，不读取登录凭据。

## WorkBuddy

- 自动来源：`%USERPROFILE%\.workbuddy\projects\**\*.jsonl`。
- 已在本机真实日志核实：顶层 `type` 为 `message`（且 `role=assistant`）或 `function_call`，`timestamp` 为 Unix 毫秒。
- 实际模型取 `providerData.model`，身份取 `providerData.conversationRequestId` 与 `providerData.messageId`，缺失消息 ID 时仅回退记录自身 `id`。不从当前配置补历史模型。
- 优先读取 `providerData.rawUsage.prompt_tokens` / `completion_tokens`；缓存和推理使用同一计量对象中的明确字段。`message.usage` 是冗余归一化副本，不叠加两份用量；仅 rawUsage 缺失时回退读取该副本。
- `rawUsage.credit` 不作为金额，也不换算为 Token。日志没有明确 provider 时显示未知。
- 本机只读扫描验证：13 文件、708 个稳定身份、4 个实际模型；1 条无法解析的完整记录被排除并报告。增量去重由统一 Store 处理。

## DeepSeek Harness

- 自动来源：`%USERPROFILE%\.dsh\sessions`，支持明文 `session.v4.jsonl` 和默认压缩 `session.v4.jsonl.zstd`。
- 只读 v4 格式。忽略同目录旧 `session.jsonl.zstd`，避免把升级前后备份重复统计；未来格式版本不猜测解析。
- `session` 头提供会话身份，`assistant/message.data.usage` 提供实际计量，`time` 为 Unix 毫秒。
- `inputTokens` 是未缓存输入，完整输入应为 `inputTokens + cacheReadTokens + cacheWriteTokens`；`reasoningTokens` 已包含在 `outputTokens` 中，不能再加一次。
- 模型和 provider 按事件顺序从当时的 `request/header.data.header.config` / `request/context` 恢复。缺字段保留未知，不读取今天的配置。
- 消息 ID 独立于文件路径，可对复制日志去重；seeded 会话的继承前缀不计入新会话，避免 fork 重复。无明确计量的失败 attempt 不补造 Token。
- 使用官方 Zstandard 1.5.7 只解压静态库，原始日志、压缩文件和会话数据库均不写入、不转换。
- 本机只读扫描验证：8 个压缩日志、658 个有计量的消息及 5 个带明确零用量的 attempt，共 663 个身份、6 个记录模型（含零用量 attempt 的路由），无解析错误。

主要格式来源：[官方会话事件文档](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/subsystems/session.md)、[官方 TokenUsage 类型](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/llm/llm/src/types.ts)、[官方持久化文档](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/subsystems/persistence.md)、[官方压缩流记录结构](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/llm/llm/src/assistant-stream.ts)。

## CodeBuddy、TRAE、Qoder、MiniMax

- CodeBuddy：读取 `.codebuddy/projects` 中符合已经核实的 WorkBuddy 消息计量结构的日志。本机仅发现进程监控日志，没有用量记录，不能声称经过 CodeBuddy 的真实会话验证。
- TRAE：本机 `Trae CN/ModularData/ai-agent/database.db` 无法按普通 SQLite 读取，未尝试解密；需要带真实用量的官方或规范导出。TraeCode / TraeWork 保持不同来源身份。
- Qoder：本机 `.qoder-cn/cache/projects/.../conversation-history` 缓存仅有角色与消息内容，没有可计量字段，不使用文本推算。
- MiniMax Code / Design：没有可核实的本机原生用量样本，使用规范用量导入；图像、视频、任务积分不转换为文本 Token。

这些产品在来源目录中保留目标与明确状态，只有实际导入或采集到用量才出现在统计界面。

## 验证

`tests/domestic_tests.cpp` 覆盖权威/冗余用量择一、缓存分项、无计量/积分不转换、非法数字、模型切换、压缩解码、旧版备份排除、未来版本拒绝、fork 继承前缀、未完成尾行与取消。

`tests/fixtures/dsh-session.v4.jsonl.zstd` 为人工构造的最小合成记录，不含用户会话内容。官方 Zstandard 源码固定在 v1.5.7，来源、生成命令与 SHA-256 位于 `vendor/sources.json`。
