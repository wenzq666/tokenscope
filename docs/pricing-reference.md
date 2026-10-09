# 公开费率参考与计费口径

核对日期：2026-10-09。实现位置：`src/pricing.h`、`src/pricing.cpp`。这是依据真实 Token 数量计算的 **标准 API 价格估算**，用于比较用量价值，不代表 Codex、Claude、Cursor、Kimi 等订阅套餐的实付金额、扣款金额或剩余额度。

## 参考项目与数据归属

用户指定的 [missuo/tokens](https://github.com/missuo/tokens) 提供了接入方式和计费设计参考，重点审计了 [`pricing/`](https://github.com/missuo/tokens/tree/main/cli/tokens-core/src/pricing) 的 `litellm.rs`、`models_dev.rs`、`openrouter.rs`、`lookup.rs`、`mod.rs` 和 `aliases.rs`。TokenScope 保持原生 C++/Win32；本次计费模块重新实现，没有引入其 Rust、Node/Bun 运行时，也没有复制其定价实现代码。

| 来源 | 单位与内容 | 本地缓存 |
| --- | --- | --- |
| [LiteLLM 公开价目](https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json) | USD / Token，输入、输出、缓存、长上下文费率 | `pricing-litellm.json` |
| [models.dev API](https://models.dev/api.json) | USD / 百万 Token，按提供商区分，支持显式上下文档位 | `model-catalog.json` |
| [OpenRouter Models API](https://openrouter.ai/api/v1/models) | USD / Token，OpenRouter 路由自己的公开输入、输出、缓存费率 | `pricing-openrouter.json` |
| [Cursor 官方价格表](https://cursor.com/docs/models-and-pricing) | Composer 2.5 标准、Fast 的核验费率 | 随 EXE 的小型参考表 |

EXE 内置 2,011 条“提供商＋模型＋价目来源”记录：367 条 LiteLLM、1,642 条 models.dev，以及 2 条 Cursor 官方核验记录。它们包含同模型的多来源记录，并不是 2,011 个互不重复的模型，更不表示本机使用过这些模型。内置 JSON 数据约 280 KB；联网可加载完整目录，2026-10-09 实际下载样本解析到 11,427 条价目记录。统计列表只显示真正采集到的模型。

内置数据由 `scripts/update-pricing-reference.ps1` 生成。原始公开 JSON 的 SHA-256、下载日期、记录数、生成文件摘要见 `vendor/pricing-sources.json`。LiteLLM 和 models.dev 的 MIT 许可证保存在 `vendor/LICENSE.pricing-litellm`、`vendor/LICENSE.pricing-models-dev`，并随应用第三方声明分发。

## 查找规则

1. 手动价格规则优先，由 Store 按软件、实际模型 ID、实际提供商隔离。
2. 自动价格先限定记录中的提供商，再查模型的完整 ID。OpenRouter 的 `anthropic/…` 是 OpenRouter 的价格身份，不能拿来给 Anthropic 直连请求计费；Azure、Bedrock、Vertex、国内外不同服务端点也不会合并为一个提供商。
3. 只接受已知提供商的等价拼写，以及 Claude `4.6`/`4-6`、明确的括号推理档位等有限拼写归一化。未知网关前缀、未知后缀、模型版本、日期快照、Fast 模式不会通过模糊相似度猜成另一个模型。
4. 提供商未记录时，可以从明确的第一方模型系列取得参考价格，`inferredProvider` 标记为推断。任意明确的自定义网关提供商不会自动继承第一方价格。`auto`、`default`、`router` 等路由标签不是具体模型。
5. 相同提供商的同名模型存在多份目录记录时，完整 ID 匹配优先；其次使用明确上下文档位和已报价类别更完整的记录，最后按确定次序选择。保留 `source`、`sourceUrl`、`matchedKey`、`fetchedAt`，以便追溯。
6. `github-copilot`、已知 coding/token plan、`kenari` 等订阅命名空间的零价记录不用于把套餐中的实际模型消耗估成免费。明确公开的免费模型费率仍可为零；缺失费率始终是未知。

实际模型 ID 原样保存在用量记录并展示，计费查找使用的归一化 ID 不会重命名用量模型。

## Token 分桶与整数计算

TokenScope 的 `input` 已包含缓存读取和缓存写入，`output` 已包含 reasoning。普通输入桶为 `input - cacheRead - cacheWrite`，普通缓存写入桶为 `cacheWrite - cacheWrite1h`。各桶各付一次费用；reasoning 默认按输出价格计费，只有目录明确给出不同 reasoning 费率时才从输出桶划出单独计费。

所有费率在读取时统一为“USD 微美元 / 百万 Token”。计算使用受限整数运算，拆分商与余数避免乘法溢出，合并整次请求后四舍五入至微美元；不会把每个桶先四舍五入再相加。人民币只影响展示汇率，底层账目保持 USD。

出现以下情况保持“未知费用”：模型或价格未匹配，存在只有总量的未分类 Token，实际使用了目录没有报价的缓存类别，计数矛盾，或者计算超过安全数值范围。未知不会转成零，也不会凭模型名称推算 Token 数量。

## 缓存与上下文档位

- [Anthropic 官方规则](https://platform.claude.com/docs/en/about-claude/pricing) 区分 5 分钟和 1 小时缓存写入。解析 LiteLLM 的显式 1 小时费率；首方 Anthropic 缺少该字段时，按核验的 2 倍输入价计算 1 小时写入。其他提供商不套用这个倍率。1 小时 Token 必须是总缓存写入的子集，剩余使用普通/5 分钟费率。
- [Gemini 官方价格](https://ai.google.dev/gemini-api/docs/pricing) 的长上下文档位由整个请求的提示词长度选择，并影响输入、输出和缓存读取。它不是“超过 200K 的那一部分单独加价”。
- [OpenAI 价格](https://developers.openai.com/api/docs/pricing) 的已声明 272K 长上下文档位按 `input > 272000` 选择整次请求费率。缓存输入参与提示词长度判断，输出长度不会自行触发输入上下文档位。
- [xAI 官方价格](https://docs.x.ai/developers/pricing) 的相应长上下文档位为 `input >= 200000`，高费率适用于整次请求。
- models.dev 新版 `cost.tiers` 中明确标为 `context` 的档位可支持 32K、100K、128K、256K、272K、512K 等阈值。若同时存在旧 `context_over_200k` 别名，优先使用明确的实际阈值；不能把 MiniMax 的 512K 档误当 200K。

这些细节与参考项目通用的逐桶阶梯计算存在有意差异：本项目按核验的厂商整请求上下文口径处理，且不会再次把缓存或 reasoning 加到已包含它们的输入/输出上。

## 官方核验覆盖与边界

2026-10-09 Cursor 官方公开的 Composer 2.5 为输入 $0.50、输出 $2.50、缓存读 $0.20 / 百万 Token；Fast 为 $3.00、$15.00、$0.50。参考项目的 Fast 硬编码仍为另一组数值，因此没有照抄。官方缓存写一栏的横线保留为未知。该补充表限定 Cursor 软件/提供商，旧 Composer 型号没有未经核验地继承现型号价格。

自动估算使用公开标准 Token 价格。日志没有记录的 Batch/Flex/Priority、地区加价、套餐折扣、企业合同、Cursor 团队附加费、Max Mode、搜索/工具次数、图像/音频等额外账单项目无法据此精确还原。无需读取账号凭据或上传对话来获取公开价目。网络与缓存管理由应用层处理；离线内置表及已有有效缓存仍可用。

历史自动价目用 `tokenscope.pricing.v1` 序列化，保存完整费率、档位、来源及获取日期。`PricingFingerprint` 用于识别价目版本变化和意外数据损坏，不是密码学签名。Store 固定历史已选价目，新公开价格或手动价格只补全尚未定价的记录，不把刷新后的价格悄悄覆盖历史。

## 验证

`tests/pricing_tests.cpp` 覆盖提供商隔离、保守模型匹配、输入/输出包含口径、5 分钟/1 小时拆分、上下文边界、任意阈值、差异 reasoning 价格、缺失费率、明确零价、订阅命名空间排除、溢出、完整请求舍入、价目序列化和来源追溯。三个下载的真实公开目录也已在该测试入口读取验证。

重新生成内置表：`./scripts/update-pricing-reference.ps1 -Download -RetrievedAt YYYY-MM-DD`。联网请求只用于公开 JSON 和许可证；生成器不执行任何下载的代码。
