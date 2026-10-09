# 国内个人版 TraeCode 用量研究

核查日期：2026-10-09。对象为国内个人版 TraeCode IDE / SOLO。**TokenScope v0.4.0 尚未实现国内账号连接；本记录不改变产品或 EXE。**

## 已有证据

- 官方个人入口：登录 [TRAE 国内官网](https://www.trae.cn/)，点击头像 → 用量管理，查看积分详情和用量明细。[官方订阅管理文档](https://docs.trae.cn/ide_subscription-management) 明确记载这一入口。
- 用户页面观察：用户本次确认本人页面只展示积分/次数和模型名称。这是页面观察，不能证明后台响应绝无 Token。
- 未登录公开前端：本项目核查了 [官方 Dashboard 页面脚本](https://lf-cdn.trae.com.cn/obj/trae-com-cn/trae_website_prod_cn/static/js/async/dashboard/page.d2f6124cb1.js) 及其共享模块，没有登录账号或发送带凭据的用量请求。页面调用 `QueryUserUsageGroupBySession`，统一 `credits` 分支传 `usage_type:[7]`，旧分支省略 `usage_type`；SDK 为 POST `https://api.trae.cn/trae/api/v1/pay/query_user_usage_group_by_session`，请求包装使用 `Cloud-IDE-JWT`。

本地核查文件位于 `build/reference-trae-cn/`：`dashboard-page.js` 体现分支参数和页面展示；`shared-58867.d6b085932a.js` 体现请求路径与字段；`shared-65960.ebe9121008.js` 体现国内 API 域名；页面及 `shared-26138.027e40a9bd.js` 体现鉴权包装。这些文件是公开前端证据，不是用户用量响应，也不随产品分发。

## 尚不能推出的结论

网页存在个人用量查询，不代表 TraeCode 的实际输入/输出 Token、缓存读写包含关系或逐模型计量已核实。公开页面显示积分、次数、模型、来源，不足以按积分推算 Token；统一积分分支的 `[7]` 也不能当作国际 TraeCode `[5,6]` 的替代参数。原始响应、真实计量语义和版本稳定性仍未验证。

[企业 OpenAPI](https://docs.trae.cn/enterprise_trae-cn-enterprise-api) 仅向旗舰版企业开放，[企业鉴权](https://docs.trae.cn/enterprise_authentication) 用应用凭据换 Bearer 访问令牌；这不是国内个人账号的接入方式。

## 后续验证所需

无需提供 JWT、Cookie、账号密码或完整请求头。若继续核查，只需本人用量页的脱敏 Request Payload / Response：删除凭据、账号/会话标识和聊天预览，保留非敏感字段名及计量结构。随后确认实际 Token 分项、缓存是否已含在输入中、多模型归属、统计时间/分组粒度和稳定身份，再决定能否实现适配。没有这些证据时保留未接入状态，不生成虚假用量或国内授权入口。

现有功能见 [TRAE 接入说明](trae-setup.md) 与 [账户连接边界](connections.md)。
