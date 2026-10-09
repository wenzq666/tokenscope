# 账号连接与只读用量同步（v0.4.0）

核查日期：2026-10-09。TokenScope 的“授权连接”入口只提供已实现真实请求的 **Cursor** 与 **TRAE 国际版 TraeCode / IDE / SOLO**。这是用户手动提供会话凭据后的本机只读同步，不是 OAuth 登录，也不是点击后仅打开网站。Copilot 继续按说明启用并落盘 OpenTelemetry；其他软件使用各自日志或真实用量导出，不显示未实现的授权按钮。

## 实际操作

1. 打开“设置 → 来源状态”，找到 Cursor 或 TRAE · TraeCode，再选择“授权连接”。需要取得凭据时，使用入口打开官方网页，系统默认浏览器负责登录。应用不读取其他软件的凭据数据库，也不接收账号密码。“添加 / 导入用量来源”仅用于选择本地文件或目录。
2. 在本人浏览器开发者工具中取得下表对应的会话值，仅粘贴到 TokenScope 的本机密码输入框；不要粘贴到聊天、截图或用量导出中。
3. 确认后，应用用原生 WinHTTP 请求真实用量。只有服务端成功返回合法用量结构、全部分页完成并写入缓存后才显示连接成功。合法空列表显示“暂无可采集用量”，不虚构软件用量。
4. 会话过期后重新输入本人取得的新凭据。当前不获取 refresh token，也不自动刷新登录会话。断开连接清除本机保存的连接凭据；已有统计历史的保留策略由应用控制。

| 来源 | 输入值 | 实际鉴权 | 同步范围 |
| --- | --- | --- | --- |
| Cursor | 登录 Dashboard 后的 `WorkosCursorSessionToken` Cookie **值**，也接受单独的 `WorkosCursorSessionToken=…` | `Cookie: WorkosCursorSessionToken=…`，固定 `Origin: https://cursor.com` | 个人用量事件；本地保留最近 30 天的真实 Token 明细 |
| TRAE 国际版 | 本人国际账号请求的 `Authorization` 中 JWT 本体，也接受 `Cloud-IDE-JWT …` | `Authorization: Cloud-IDE-JWT …`，**不是 Bearer** | 同一账号 IDE / SOLO 主要聊天类型 `[5,6]`，最近 30 天；不包含辅助类型、国内版或 TraeWork |

Cursor Cookie 不是 Cursor Agent API Key；TRAE 会话 JWT 不是模型 API Key、会话 `session_id` 或 `tokens.ci` 读令牌。输入框不接受其他 Cookie 拼接或额外请求头，避免误用完整请求。输入的凭据由应用层用 Windows DPAPI 保护；本模块不会把凭据写入 JSON 缓存、普通设置或日志。

## 请求与来源依据

| 来源 | 固定只读查询地址 | 请求参数与分页 |
| --- | --- | --- |
| Cursor | `https://cursor.com/api/dashboard/get-filtered-usage-events` | POST JSON：`teamId:0`、`page` 从 1 开始、`pageSize:500` |
| TRAE 国际版 | `https://api-sg-central.trae.ai/trae/api/v1/pay/query_user_usage_group_by_session` | POST JSON：`start_time/end_time` Unix 秒、`page_size:20`、`page_num` 从 1 开始、`usage_type:[5,6]`；页间隔 300ms |

这些个人 Dashboard 协议来自用户指定的 [missuo/tokens Cursor 实现](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/cursor.rs) 与 [TRAE 实现](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/trae.rs)，是对其原始用量请求及缓存结构的独立 C++ 实现。本次使用的参考 ZIP 为 2026-10-09 快照，SHA-256：`ff2159da7fb8b283b6075d8357fc22fdc8580babefd0a673f8eb8f82fa0f6c84`，上游 MIT 归属见现有第三方声明。没有引入 Rust、Bun、Node 或 npx 运行时，也没有复制其凭据解密、登录或刷新实现。

Cursor [官方公开 Admin API](https://cursor.com/docs/account/teams/admin-api) 另有团队鉴权与 `POST /teams/filtered-usage-events`，它使用团队 API Key 的 Basic Auth，不能拿来证明个人 Cookie 接口有公开稳定性保证。本次仅实现上述个人 Dashboard 协议，不把其会话接入宣传为官方 OAuth 或通用公共 API。TRAE 此会话查询同样缺少本次可核实的官方公开 API 契约；协议可能随服务升级变化，模块将报格式错误并保留原缓存，不能把异常响应当空用量成功。

Cursor 参考代码核实的个人查询参数没有明确日期过滤字段，因此本项目只发送已核实的三个参数，在本机按真实毫秒时间筛选 30 天。若服务返回更长的用量历史，仍需分页读取这些**用量元数据**才可确认完整性；不猜测服务的排序、不用遇到一条旧记录就提前结束，也不添加未经核实的 `startDate/endDate`。访问的始终是用量接口，不调用聊天正文、文件或项目内容接口。

TRAE 国际账号查询不能套用于国内站。当前没有国内个人版、国内企业版或 TraeWork 账号连接，国内个人与企业用量入口见 [TRAE 接入说明](trae-setup.md)。

## 国内个人版 TraeCode：已有网页查询，尚未接入

国内个人版 IDE / SOLO 用户可登录 [TRAE 国内官网](https://www.trae.cn/)，通过头像 → 用量管理查看积分详情和明细；这是 [官方个人订阅文档](https://docs.trae.cn/ide_subscription-management) 记载的入口，不要求企业旗舰版。企业 [OpenAPI](https://docs.trae.cn/enterprise_trae-cn-enterprise-api) 与 [应用鉴权](https://docs.trae.cn/enterprise_authentication) 面向旗舰版企业，其 Bearer 体系不能用于国内个人账号。

2026-10-09 未登录公开前端核查确认，个人 Dashboard 的 `QueryUserUsageGroupBySession` 对应 `https://api.trae.cn/trae/api/v1/pay/query_user_usage_group_by_session`，使用 `Cloud-IDE-JWT`；统一积分分支传 `usage_type:[7]`，旧分支不传该字段。这是网页查询实现的证据，不能把国际版 `[5,6]` 直接搬到国内，也不能证明国内 TraeCode 已有可统计的实际 Token 分项。

用户本人当前页面只见积分/次数和模型名称；该页面观察不等于后台响应绝无 Token。尚需用脱敏响应确认实际 Token、缓存包含关系、多模型归属和时间/身份粒度，v0.4.0 因此仍不提供国内授权按钮。本次不需要用户提供 JWT/Cookie；若继续核实，只需删除凭据、账号/会话标识和聊天预览后的 Request Payload / Response。公开证据及边界见 [国内个人版研究记录](trae-cn-research.md)。

## 完整性与缓存

- 默认最多 100 页、累计 32 MiB、每页最多 8 MiB、整体 120 秒；单次 WinHTTP 阶段超时最多 15 秒。可注入测试配置的上限为 500 页、64 MiB、120 秒。耗尽上限会失败，不保存部分结果。
- Cursor 的 `totalUsageEventsCount` 优先决定是否结束，服务把一页限制得比请求更短时仍继续；没有总量时参考 `hasNextPage` 或短/空页。TRAE 没有总量时继续到空页，不能因为第一页不足 20 条就截断。
- 响应必须包含对应的用量数组，明确的有效时间、请求/会话身份与实际 Token 分项。重复页、变化的总量、提前空页、矛盾的分页标志、重复 JSON 字段、负数/非整数计数、只有金额或总次数的记录不能冒充完整 Token 明细。Cursor 明确 `isTokenBasedCall:false` 的旧次数计费事件可以跳过，不能按 Credits 反推 Token。
- 保存格式保持现有采集器所需的原生结构：Cursor 为 `usageEventsDisplay` 对象，TRAE 为会话数组。只保留身份、时间、实际模型和输入/输出/缓存计数白名单；模型 ID 原样保留。不会把缓存读写再加到费用之外重复计数，现有采集器统一含缓存输入口径。
- 目标为调用方提供的私有目录下 `usage.json`。全量分页校验完成后，先写临时文件、刷新到磁盘，再原子替换；失败/取消保留之前有效文件。每目录有互斥文件句柄，避免两个同步交错写入。历史记录仍由 Store 去重和持久化，刷新最近 30 天不会删除此前数据库中的历史。

HTTP 仅允许两个固定 HTTPS 地址，保持系统证书校验，关闭重定向、自动 Cookie 管理及环境账号自动鉴权。会话令牌只出现在对应厂商的请求头，不放 URL、POST JSON、错误提示或结果对象。非 200 响应连正文也不留存；错误只使用固定提示与数值状态码。没有凭据就拒绝发起请求。取消会在分页、接收与写入边界检查；正在等待的同步 WinHTTP 调用受阶段超时约束。

模块在作用域结束时通过 `SecureZeroMemory` 尽力擦除规范化令牌、请求头与 WinHTTP 宽字符头缓冲区的自有副本；调用方仍需清理密码输入控件和自己持有的凭据。这不构成对整个进程内存中所有历史副本的绝对擦除保证。

## 接口与验证边界

实现为 `src/connections.h/.cpp`。应用后台线程调用 `SyncConnection(sourceId, token, destinationDirectory, cancelled)`；成功返回原生缓存路径，由应用登记到对应采集器。模块不管理窗口、DPAPI 凭据、启动同步策略或数据库。测试可注入 `ConnectionTransport` 与 `ConnectionSyncOptions`；`BuildConnectionRequest` 为纯请求计划，便于验证厂商、鉴权、日期单位和分页。

`tests/connections_tests.cpp` 验证 Cookie 与 JWT 的隔离、无凭据/非法请求拒绝、分页与日期范围、原生采集器兼容、Auto 未知模型、缓存分桶、空账号、格式变化、鉴权失败、服务器正文不泄露、重复/不完整分页、字节/页数/时间上限、取消、异常和 Windows 原子替换失败后的旧缓存保留。fixture 不含真实账号凭据。

目前没有用户会话凭据，**未实际登录 Cursor 或 TRAE 账号做端到端服务端验证**。已完成的验证是参考协议审计、原生 WinHTTP 实现联编和注入响应测试；真实账号首次连接必须以应用收到并验证的响应为准。
