# Tokens 参考与本项目实现差异

核实日期：2026-10-09。用户指定参考 [missuo/tokens](https://github.com/missuo/tokens)，该项目基于 [junhoyeo/tokscale](https://github.com/junhoyeo/tokscale)，以 MIT 许可证分发。

本轮保存的参考快照为 2026-10-09 的 `main` ZIP，SHA-256：`ff2159da7fb8b283b6075d8357fc22fdc8580babefd0a673f8eb8f82fa0f6c84`。用保存的 ZIP 摘要定位本次版本，不把可变 `main` 链接当不可变提交。未执行下载项目的脚本。Tokens / Tokscale 的版权和 MIT 许可证保留在 `vendor/THIRD-PARTY-NOTICES.txt`。

TokenScope 继续使用 C++ / Win32，把明确的字段、读取路径、计费口径和展示思路适配到本地软件，不引入 Tokens 的 Rust CLI、Node/Bun 环境、网页排行榜或上传功能。v0.3.1 属于历史验证基线；当前 v0.4.0 大小和验证结果见 [构建报告](../dist/build-report.json) 和 [发行版状态](development-status.md)。

## 用量来源

参考 README 列出 41 个客户端。本项目合并原有国产和云端来源后提供 **56 个入口**，分类如下：

| 范围 | 原生格式解析 | 已有同步缓存 | 需真实用量导出 |
| --- | --- | --- | --- |
| 参考 README 的 41 项 | 27 | 3 | 11 |
| TokenScope 的 56 项 | 28 | 3 | 25 |

参考清单之外额外的原生来源是 DeepSeek Harness。Qoder 当前缓存无计量，不计入原生解析。28 项原生格式表示有条件的解析实现，不代表每项均有本机真实样本，也不涵盖所有历史版本。全部目录来源都另有规范实际用量导入入口。

| 路径 | 本版处理 |
| --- | --- |
| 原生日志 / 数据库 | 在明确应用目录内只读采集真实请求身份、时间、模型和 Token，要求符合已核实格式。Copilot 须启用并实际落盘请求级 OTel。 |
| 已有同步缓存 | Cursor、TRAE、Antigravity 读取磁盘上已经存在的 Tokens 同步缓存；不登录、不读取 Cookie、不触发账号同步。 |
| 官方导出 / 遥测 | 读取 Cursor CSV、Copilot OTel / SDK 等明确字段，不把会话汇总或 Credits 换成 Token。 |
| 规范导出 | 全目录接受 `tokenscope.usage.v1`；OpenAI / Anthropic API、Dify、Coze 另有已核实结构。 |
| 缺少可靠计量 | 来源管理说明所需导出，用量统计保持为空，不生成示例数据。 |

参考客户端中 **11 项明确需要真实导出**：Crush、Devin Desktop、Droid、Fx、Goose、Hermes Agent、Kiro、Mux、Orca、Warp、Zed Agent。其现有上下文快照、会话累计、费用或共享 hook 副本不足以安全还原逐日 Token；本版不以字符长度、文件修改时间或最后请求时间分摊。

原有其他 14 项导出来源为 ChatGPT、TraeWork、Manus、Gemini Deep Research、Kimi 网页、MiniMax Code、MiniMax Design、Coze、Dify、n8n、阿里云百炼、智谱智能体平台、OpenAI Agents 和 Qoder。云端平台只有实际 API usage 或可信用量导出才进入统计。

具体路径、版本条件、去重证据和边界见 [扩展来源清单](extended-adapters.md)、[消息家族审计](extended-families.md)、[CLI 适配](cli-adapters.md) 和 [国产适配](domestic-adapters.md)。CodeBuddy 当前为共享格式验证；OpenClaw 跳过不完整 Codex 镜像；Antigravity CLI 逆向结构须有真实逐次时间和响应身份；OpenCodeReview 缓存口径不明时拒绝。新增名称不会仅因进入目录就变成采集成功。

模型统计没有固定白名单。真实记录出现新模型 ID，即可生成软件内的模型行；未记录模型或只有 Auto 路由标签时保留未知。价目目录认识模型，与软件能否取得真实用量，是不同条件。

## 计费

借鉴参考项目多来源设计，使用 LiteLLM、OpenRouter、models.dev 三份公开目录。内置 **2,011 条价目记录**是“提供商＋模型＋来源”的报价行数，包含同模型多源报价，不是独立模型数。数据、日期、许可和摘要见 `vendor/pricing-sources.json`。

TokenScope 的输入已包含缓存、输出已包含推理，计费前分成互斥桶，不能照搬分桶相加。明确的 1 小时缓存 Token 是缓存写入子集，按独立费率计费；没有可靠分项或费率时保持未知。上下文档位按整个请求输入选择输入、输出和缓存费率，不仅对超过阈值的剩余 Token 加价。

相同模型仍限定实际提供商，不随意互用 OpenRouter、Azure、Bedrock、自定义网关或国内外端点价格。安全拼写归一化只用于价格查找，模型 ID 保持原样。手动规则对新请求和未定价记录优先；已定价历史及同请求后续增长继续使用固定价目。

参考硬编码也作当日核实。例如 [Cursor 官方表](https://cursor.com/docs/models-and-pricing) 当前 Composer 2.5 Fast 为输入 $3、输出 $15、缓存读 $0.50 / 百万 Token，与参考项目保存的另一组数值不同，因此采用官方核验数据。无明确报价的缓存写入保持未知，没有把横线猜成零。

费用均是 **标准 API 价格估算**，不等于订阅实付或套餐余量。没有对应元数据，无法还原调用模式、地区加价、企业折扣、团队附加费、搜索/工具、图像/音频等完整账单。厂商依据、分桶、舍入和持久化细节见 [公开费率参考](pricing-reference.md)。

## 联网行为

用户在设置主动更新，或明确传入 `--network-check` 时，才下载三家的公开 JSON 价目。请求不发送日志、对话、软件清单、Cookie 或 API 密钥。响应校验后替换缓存，失败保留已有可用数据；刷新只补未定价记录，不改写固定的历史费率。

联网价目能补充模型报价，不能安装新采集器，也不访问远端账号。全新智能体搜索、登录同步与可信适配器动态更新仍需后续实现。v0.3.1 已另接入 [Frankfurter / ECB 自动公开汇率](exchange-rate.md)：未填写手动值时按日异步检查，不上传用量。

## 展示

参考分布展示，增加软件/模型占比条，以及普通输入、普通输出、缓存读、缓存写、推理和未分类的互斥分桶。缓存和推理可单独观察，同时不会虚增总量。同软件内按模型分行，保留未知模型及部分未知费用状态。

图表遵循用户确定的交互：进入主窗口先展示趋势；点击任一天只改变选中状态和下方用量，不移动趋势区间或热力网格。趋势日期仅由显式翻页/范围控件改变，热力图截至今天按日推进，未采集日期为空白格。

使用 38 个品牌图标资源，缺少专属资源的来源使用通用图形，出处见 `design/assets/brands/sources.json`。托盘保留图标、悬停摘要、打开主窗口与右键菜单，已移除托盘面板。USD / CNY 和图表颜色由设置控制。
