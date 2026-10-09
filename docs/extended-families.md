# 扩展消息家族适配审计

实现：`src/adapters/extended_families.cpp`。审计日期：2026-10-09。

仅采集来源文件真正记录的请求身份、时间、模型、提供商与计量。不会用正文估算 Token，不会把 credits 当货币，不会从当前设置或正文标签倒推历史模型，不会用文件修改时间补请求日期。未知模型和提供商保留空值。

| 来源 | 接受的原生文件及记录 | 计量与身份 |
| --- | --- | --- |
| Cline、Roo Code、Kilo Code | VS Code `globalStorage/<extension>/tasks/<task>/ui_messages.json` 中 `type=say, say=api_req_started`；`text` 是请求计量 JSON | 真实 `ts`；每条 `modelInfo.modelId/providerId`；`requestId` 或事件 `id` 优先。缺少 ID 时使用任务目录名、记录时间和同时间记录序号。 |
| Cline CLI | `~/.cline/data/sessions/<session>/<session>.messages.json` 的 assistant `messages[].metrics` | 真实 `ts`、消息 `id`、每条 `modelInfo.id/provider`。`inputTokens` 已含缓存，不再次相加。 |
| Amp | `~/.local/share/amp/threads/*.json` 的 `usageLedger.events[]` | 使用事件真实 `timestamp` 和 `model`；`tokens.input` 加独立 `cacheReadInputTokens/cacheCreationInputTokens`。事件 ID 优先；否则用 thread ID、时间、操作、from/toMessageId 及同身份序号。仅有 message usage、无逐次真实时间的旧记录不纳入。 |
| Pi | `~/.pi/agent/sessions/**/*.jsonl`；Pi 格式消息 | 仅 assistant `type=message`。`responseModel` 优先于请求别名 `model`；`responseId` 优先，后备事件 ID 与时间、模型、提供商，不依赖会话路径，可跨 fork 去重。`input` 加独立缓存，`reasoning` 是 `output` 子集。 |
| OpenClaw | `~/.openclaw/agents/<agent>/sessions/*.jsonl`；当前 `agent/openclaw-agent.sqlite` 的 `transcript_events.event_json` | 同消息格式，优先消息真实 `timestamp`；`reasoningTokens` 是输出子集。事件 ID、真实时间及模型身份支持 SQLite/JSONL 迁移和 fork 去重。跳过 delivery mirror、gateway notice。 |
| Codebuff | `~/.config/manicode[-dev/-staging]/projects/*/chats/*/chat-messages.json` | 仅 assistant/ai/agent 的 `metadata.usage` 或 `metadata.codebuff.usage`；要求消息 ID 和真实 timestamp/createdAt/metadata.timestamp。支持明确 Anthropic 与 OpenAI 字段形态；不解析反复包含旧消息的 `runState.messageHistory`。 |

## 缓存计算的核实与保守处理

参考 Tokens 的 Roo 家族解析器把 `tokensIn`、cache read、cache write 都视为独立桶。核对官方最终写盘点后发现，虽然界面字段同名，软件之间语义不同，不能仅按 provider 或 `apiProtocol` 判断。

- Cline UI 的 SDK translator 明确将总 inputTokens 减去缓存，再保存为 `tokensIn`；这些是独立桶，需相加。Cline CLI 的 `.messages.json.metrics.inputTokens` 则仍是总输入，不能再次叠加。
- Roo 与 Kilo 旧 IDE 的 `Task.ts` 写入 `costResult.totalInputTokens`；`cost.ts` 已把 Anthropic 缓存加进此总量，OpenAI 则本来就是总量。所有协议的已落盘 `tokensIn` 都按总输入处理，不能再次叠加缓存。

因此适配器按软件和持久化格式分别解释字段。保留真实 provider/apiProtocol 文本作为来源信息，不将其错误地用作缓存计量模式。Kilo 当前新版 IDE 已使用 OpenCode backend，其 SQLite 由 Kilo CLI 的 OpenCode 家族入口读取；这里的 tasks 适配用于历史 IDE 文件。

Roo/Cline/Kilo 未持久化的推理计量无法恢复，只报告文件实际保留的 Token 字段。旧任务只在正文 `<model>` 标签或会话清单保存模型时，不将该标签/当前模型归到全部历史请求。

## 明确限制

- Amp 没有事件 ID 的旧 ledger，使用稳定事件元数据与同身份序号；完全相同的真正独立事件依赖原 ledger 的顺序。无 thread ID 及事件 ID 的记录拒绝。
- Roo 家族无请求 ID 的历史记录需要保留任务目录名；复制时修改任务目录名无法证明仍为同一个任务。相同时间的多条记录通过原数组序号区分，顺序变更也无法保证身份。
- OpenClaw `codex-app-server:*` 镜像只有一轮末次模型响应，既可能少计，又可能与共享 Codex home 重复；本版跳过。需要把完整 Codex rollout 通过 Codex 来源导入，不能宣称 OpenClaw 的 Codex harness 全量原生支持。
- OpenClaw 缺少消息时间且事件也无真实时间的记录拒绝，不能用 SQLite 写入时间或文件修改时间当消耗日期。不会用会话当前模型回填历史模型。
- Droid `*.settings.json.tokenUsage` 是会话累计；参考实现按正文长度分摊日期或使用修改时间，这不是实际逐日计量，本版不采用。Mux `session-usage.json.byModel` 也是累计，`lastRequest.timestamp` 无法确定累计 Token 的日期，不归到最后一次调用当天。两者保留规范逐次用量导入入口。
- 任何零占位、负值、非整型、计量溢出、缓存超出输入、推理超出输出、已提供的总量与拆分不一致都会被跳过或计入无效记录。只有总量的明确 usage 可保留到 `unclassified`，不会假拆输入/输出或误定价。

## 依据

- [Tokens 家族解析器目录](https://github.com/missuo/tokens/tree/main/cli/tokens-core/src/sessions)：cline、roocode、kilocode、amp、pi、openclaw、codebuff、droid、mux。
- [Roo 官方 OpenAI normalizer](https://github.com/RooCodeInc/Roo-Code/blob/main/src/api/providers/openai-native.ts)：总输入与缓存子集。
- [Roo 官方 Anthropic provider](https://github.com/RooCodeInc/Roo-Code/blob/main/src/api/providers/anthropic.ts)：输入与缓存独立。
- [Roo 官方 Gemini provider](https://github.com/RooCodeInc/Roo-Code/blob/main/src/api/providers/gemini.ts)：promptTokenCount 包括 cachedContentTokenCount。
- [Roo 官方任务持久化](https://github.com/RooCodeInc/Roo-Code/blob/main/src/core/task/Task.ts) 与 [cost.ts](https://github.com/RooCodeInc/Roo-Code/blob/main/src/shared/cost.ts)：保存已归一化的 totalInputTokens。
- [Cline 官方 SDK 界面消息转换](https://github.com/cline/cline/blob/main/apps/vscode/src/sdk/message-translator.ts)：normalizeUsageEvent 明确生成独立输入/缓存桶。
- [Kilo 旧 IDE 官方任务持久化](https://github.com/Kilo-Org/kilocode-legacy/blob/main/src/core/task/Task.ts) 与 [cost.ts](https://github.com/Kilo-Org/kilocode-legacy/blob/main/src/shared/cost.ts)：保存已含缓存的总输入。

`tests/extended_family_tests.cpp` 覆盖每个原生家族、缓存差异、真实模型、路径复制/fork/迁移身份、同时间独立事件、总量一致性、缺日期/时区、通知镜像、credits、RunState 回放以及累计文件拒绝。
