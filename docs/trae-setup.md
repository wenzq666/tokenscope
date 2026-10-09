# TRAE 用量接入说明（Windows）

核查日期：2026-10-09，适用于 v0.4.0。本文区分 TRAE 官方产品登录、TokenScope 的手动凭据连接，以及第三方 Tokens 缓存读取。TokenScope 新增 **TRAE 国际版账号只读用量连接**，由用户在本机密码输入框提供会话 JWT 后原生同步；不会自动安装 Tokens、读取其他软件的登录凭据或替用户登录。国内版和 TraeWork 暂无账号连接。

## 先确认用的是哪个产品

| 名称 | 实际含义 | 当前 TokenScope 能力 |
| --- | --- | --- |
| TRAE CN / 国内版 | 地区版本，官方入口为 `trae.cn`，不是一种编码模式；本次用户使用国内个人版 TraeCode IDE / SOLO。 | 官网有个人用量管理入口；v0.4.0 没有国内版账号直连，可导入符合要求的真实用量数据。 |
| TraeCode | AI 开发工具产品，内部可切换 IDE / SOLO 模式。国内版和国际版都有这个产品名称。 | `TRAE · TraeCode` 入口新增**国际版 IDE / SOLO 手动 JWT 连接**；也可读取已有 Tokens 用量缓存或导入规范用量。 |
| TraeWork | 独立工作台产品，有网页、桌面、移动端形态；内部的 Work / Code / Design 是模式。 | 当前仅接受真实 Token 导出，没有 TraeWork 账号直连。 |

因此，“CN 还是 Code”不是二选一：你可能用的是国内版 TraeCode，也可能用的是国内版 TraeWork 的 Code 模式。产品与模式的区别依据 [TraeCode 官方说明](https://docs.trae.cn/ide_what-is-trae-code) 和 [TraeWork 快速开始](https://docs.trae.cn/work_trae-work-web-and-desktop-quickstart)；地区官网见 [国内站](https://www.trae.cn/) 和 [国际站](https://www.trae.ai/)。

## 只是使用 TRAE 官方软件

国内 Windows 用户可以前往[国内官网](https://www.trae.cn/)下载 TraeCode，安装后按客户端提示完成初始设置和登录，再打开项目使用。TraeCode 支持 Windows 10/11 x64；官方安装步骤见[快速开始](https://docs.trae.cn/ide_get-started-with-trae)。TraeWork 则使用官网对应的 TraeWork 下载入口，或官方网页版。国际版从[国际官网](https://www.trae.ai/)下载相应产品，并在自己的客户端或官网登录。

这是使用 TRAE 的账号登录，不会自动给 TokenScope 提供用量。仅正常使用 TRAE 不需要向 TokenScope 输入任何账号材料；选择下述国际版连接时才在本人本机输入会话 JWT，始终不需要提供账号密码或模型 API Key。

## 国内个人版 TraeCode IDE / SOLO：官方用量入口

国内个人版并非只能依靠企业控制台查看消费。用系统默认浏览器打开 [TRAE 国内官网](https://www.trae.cn/)，登录本人账号后点击右上角头像 → **用量管理**，即可查看积分详情和用量明细。这是个人会员的官方入口，操作依据见 [官方订阅管理文档](https://docs.trae.cn/ide_subscription-management)。

本次用户确认自己的国内个人版页面显示积分/次数和模型名称。2026-10-09 的未登录公开前端核查也发现国内个人 Dashboard 查询链路：统一 `credits` 分支发送 `usage_type:[7]`，旧计费分支不传此字段；请求国内 `api.trae.cn`，采用 `Cloud-IDE-JWT`。这证明国内个人用量查询存在，不证明响应已经提供 TraeCode 的实际输入/输出 Token、缓存包含关系或每个模型的可靠计量；页面只展示积分也不能证明后台绝无 Token。具体证据见 [国内个人版研究记录](trae-cn-research.md)。

**v0.4.0 仍没有国内版连接实现。** 不能直接复用国际版 `[5,6]`、替换国际接口域名或把积分/次数换成 Token。无需提供 JWT、Cookie 或完整登录请求；进一步核实只需本人用量页脱敏后的 Request Payload / Response，删除凭据、账号/会话标识和聊天预览，保留非敏感字段名及计量结构。真实 Token 分项、缓存口径、多模型归属和日期身份验证通过后，才有依据设计国内版适配。

## 国际版：直接在 TokenScope 连接

1. 打开 TokenScope 的“设置 → 来源状态”，找到 `TRAE · TraeCode` → “授权连接”，按入口用系统默认浏览器打开[国际版账号用量页](https://www.trae.ai/account-setting#usage)，登录本人国际版账号。“添加 / 导入用量来源”只用于本地文件或目录。
2. 在浏览器开发者工具 Network 中找到本人的用量请求，读取 `Authorization` 值，将 JWT 本体粘贴到 TokenScope 的本机密码输入框；也接受完整 `Cloud-IDE-JWT …` 值。不要输入 `Bearer`、Cookie、会话 ID、模型 API Key 或 `tokens.ci` 令牌。
3. 确认后，由 TokenScope 原生 WinHTTP 只读查询最近 30 天的账号用量，主要聊天类型为 `[5,6]`。只有真实响应通过身份、时间、Token 计数和完整分页验证并写入缓存后才显示成功；合法空账号会提示暂无可采集用量。
4. TokenScope 用 Windows DPAPI 保护本机保存的会话凭据。会话过期需要重新输入新 JWT；当前没有 refresh token 自动续期。断开连接清除保存的连接凭据，不必卸载或退出 TRAE。

国际 IDE 与 SOLO 查询同一账号级用量，不能重复当作两套独立软件相加。该接入使用参考项目核实的国际用量接口，属于可能变化的 Dashboard 协议；并非官方 OAuth，未验证或异常响应不会显示为已连接。没有提供真实凭据时，开发阶段只能验证请求与响应 fixture，不能声称已替用户登录。完整接口、安全边界和验证情况见 [账号连接说明](connections.md)。

这条路径不需要 Node、Bun、npx 或第三方 Tokens。下面的缓存方案仍适合已经拥有同步用量文件的用户。

## 国际版：已有缓存可以直接读

本次下载的 [missuo/tokens](https://github.com/missuo/tokens) 参考源码包含国际版 TRAE 的 `trae login`、`trae status` 和 `trae sync` 命令。`--variant ide` / `solo` 选择凭据来源；两者同步同一账号级用量，不能当成两个独立软件分别相加。[授权及同步实现](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/trae.rs)

如果你已经用 Tokens 同步过，在 TokenScope 中选择“添加 / 导入用量来源” → `TRAE · TraeCode` → “选择文件夹”，选择 **sessions 子目录**。Windows 常见位置为：

```text
%APPDATA%\tokens\trae-cache\sessions
```

TokenScope 自动候选同样包含该位置，以及旧的 `%USERPROFILE%\.config\tokens\trae-cache\sessions`。若使用 `XDG_CONFIG_HOME`，兼容位置改为 `%XDG_CONFIG_HOME%\tokens\trae-cache\sessions`。若显式设置了非空 `TOKENS_CONFIG_DIR`，三个 Tokens 缓存来源均只扫描该配置根，TRAE 位置为 `%TOKENS_CONFIG_DIR%\trae-cache\sessions`，不再回退扫描其他账户目录。相同路径按 Windows 大小写与分隔符规范去重。

只读缓存无需再次授权。自动发现失败时，可手动选择实际 sessions 目录；之后刷新即可。没有真实 Token 的记录不会显示为一条零用量软件。

## 国际版：自己选择使用 Tokens 生成缓存

以下是提供给用户自行操作的可选路径，TokenScope 不执行这些命令。Tokens 是独立第三方工具，不是 TRAE 官方授权应用。

1. 先在系统默认浏览器中打开[国际版用量页](https://www.trae.ai/account-setting#usage)，登录自己的国际版 TRAE 账号。
2. 按 [Tokens README 的 Windows 说明](https://github.com/missuo/tokens#install)准备 Bun 或 Node.js 18+。在 PowerShell 中先核对本机实际发布包是否包含这些子命令：

   ```powershell
   npx --yes tokens-cli@latest --version
   npx --yes tokens-cli@latest trae --help
   npx --yes tokens-cli@latest trae login --help
   ```

3. 本次核查源码的自动凭据发现只定位 macOS 的 `Library/Application Support`；不能承诺 Windows 的普通 `trae login` 自动成功。该源码提供手动 JWT 输入方式：

   ```powershell
   npx --yes tokens-cli@latest trae login --manual --variant ide
   ```

   国际版 SOLO 的凭据来源对应 `--variant solo`。按该工具自己的提示，在本人浏览器开发者工具的 Network 中找到 `query_user_usage` 请求，读取其 Authorization 中的 JWT，**只在本人本机终端的 Token 提示处输入 JWT 本体**。若值以 `Cloud-IDE-JWT ` 开头，去掉此前缀；同步代码会自行添加它。这里不是会话 `session_id`、模型 API Key、Cookie，也不是 `tokens.ci` 的 `tkr_…` 读令牌。不要将这些凭据发到聊天、截图或用量文件中。
4. 检查状态并同步最近 30 天：

   ```powershell
   npx --yes tokens-cli@latest trae status
   npx --yes tokens-cli@latest trae sync --since 30
   ```

   确认工具实际输出同步成功和记录数量；仅显示 authenticated 表示本地保存过凭据，不等于服务端已经验证成功。默认同步主要聊天类型；`--include-aux` 才会请求辅助类型，不能据此保证所有功能和历史数据都能返回。[命令参数实现](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/main.rs)
5. 在 TokenScope 中按上一节选择 sessions 目录。这里只需要 TRAE 的本地同步缓存，不需要执行 Tokens README 中用于排行榜的 `tokens login`、`submit` 或后台上传服务。

手动 JWT 到期后需要由本人重新授权；手动方式没有可自动续期的 refresh token。本次源码会把凭据保存到 `trae-cache/credentials-*.json`，Windows 分支未使用操作系统凭据加密，手动输入也不是隐藏密码框；请只导入 sessions 子目录。停止给 Tokens 使用凭据可由本人运行：

```powershell
npx --yes tokens-cli@latest trae logout
```

该命令清除 Tokens 保存的 TRAE 凭据，不代表删除已同步的用量缓存。以上步骤基于本次核查源码，未实际登录账户或运行同步；如发布包帮助或服务端行为变化，以实际版本和官方页面为准。

## 国内企业版与真实导入边界

国内个人版应使用上面的官网个人用量入口；下述企业能力是另一套权限体系。参考 Tokens 源码只实现国际版 IDE / SOLO，没有 CN variant 或国内同步分支，不能把国内凭据交给国际命令。

国内官方企业版已说明可查看按模型、客户端和会话统计的 Token 用量，并导出输入/输出明细：个人可在企业版控制台“账户中心 → 个人用量”查看，有相应权限的管理员可在“企业配置 → 用量管理”查看。适用条件和操作入口见[查看个人用量](https://docs.trae.cn/enterprise_check-individual-usage)及[查看企业用量](https://docs.trae.cn/enterprise_check-usage-for-enterprise)。该企业版能力不能推断为所有个人账户均可使用。

[企业 OpenAPI 概览](https://docs.trae.cn/enterprise_trae-cn-enterprise-api) 限定旗舰版企业；[企业鉴权](https://docs.trae.cn/enterprise_authentication) 使用企业应用 `app_id` / `app_secret` 换取 Bearer 访问令牌。此方式不能用于国内个人版 TraeCode，也不能代替个人 Dashboard 的会话鉴权。

TokenScope 尚未适配这一国内原始导出表格或私有接口。已有真实导出时，需要先依据实文件确认字段，再转换成[规范用量格式](public-usage-export.md)，选择 `trae-code` 或 `trae-work` 归属导入。必须有真实记录身份、带时区的日期和实际 Token；仅总量可保留为未分类，不能虚构输入/输出或费用。若只是跨天会话的累计值，不能靠会话开始/结束时间把总量伪拆成每日用量。

普通聊天分享、聊天导出、积分和会话次数不能替代 Token 用量。国际缓存也只采用 API 返回的真实计量和 `usage_time`，不是每一轮请求日志；Auto 未返回具体模型时保持未知，不能保证还原全部历史、跨天调用分布或 TraeWork 的独立消费。

## 核查依据

- TRAE 官方产品、安装、个人订阅/用量入口和企业用量/OpenAPI 文档链接均列在对应段落；个人 Dashboard 的公开前端证据与未证实项见 [国内个人版研究记录](trae-cn-research.md)。
- Tokens 主项目 [README](https://github.com/missuo/tokens)、[TRAE auth/sync](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/trae.rs)、[命令参数](https://github.com/missuo/tokens/blob/main/cli/tokens-cli/src/main.rs)、[Windows 配置路径](https://github.com/missuo/tokens/blob/main/cli/tokens-core/src/paths.rs)。
- 本次本地审计包为 `build/reference/tokens-main.zip`，包内 CLI 版本为 `27.1.1`；SHA-256：`FF2159DA7FB8B283B6075D8357FC22FDC8580BABEFD0A673F8EB8F82FA0F6C84`。网页 `main` 与 npm `latest` 后续可能变化，版本差异需要重新核实。
