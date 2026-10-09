# 美元／人民币自动参考汇率

核对日期：2026-10-09。自动汇率使用 [Frankfurter 官方 API](https://frankfurter.dev/) 的明确 ECB 单币对接口：`https://api.frankfurter.dev/v2/providers/ecb/rate/usd/cny`。服务无需 API key，返回美元兑换人民币 **CNY** 的最新已发布日参考价，不使用离岸人民币 CNH，也不声称为实时成交汇率。

Frankfurter 当前 v2 默认可混合多个机构的数据；本项目在 URL 中指定 `providers/ecb`，保持来源一致。底层 [欧洲中央银行参考汇率](https://www.ecb.europa.eu/stats/policy_and_exchange_rates/euro_reference_exchange_rates/html/index.en.html) 通常在工作日欧洲中部时间约 16:00 发布，节假日不更新。周末或当天尚未发布时，最近公布日期可能早于电脑日期，界面应展示响应中的汇率日期与 `Frankfurter / ECB` 来源，而不是把下载时间当成价格日期。

实际下载样本（2026-10-09 获取）：

```json
{"date":"2026-10-08","base":"USD","quote":"CNY","rate":6.7023}
```

2026-10-08 ECB 欧元参考价为 USD 1.1186、CNY 7.4972；交叉转换为 1 USD ≈ 6.7023 CNY，与该接口一致。这是公开参考换算，不是银行结汇、支付渠道或信用卡的实际入账汇率。

## 模块与校验

`src/exchange.h` / `src/exchange.cpp` 只处理常量和纯 JSON 解析，不访问网络、磁盘、账号或系统时间。`ExchangeRateQuote` 返回 `usdToCny`、真实发布日期 `date` 和固定来源 `source`；来源不由响应任意文本控制。

解析器拒绝空文档、超过 64 KiB 的响应、非法或多段 JSON、重复关键字段、非 USD 基准、非 CNY 目标、非法公历日期、缺失字段、字符串或非有限数值费率。USD/CNY 的宽泛有效范围为 0.01–1000，仅用于拦截损坏数据，不作价格预测。日期最早为 ECB 欧元参考汇率起始年份 1999。是否来自未来、是否需要刷新由应用层结合当前日期判断，纯解析器不读取时钟。

兼容 [Frankfurter v1](https://frankfurter.dev/v1/) 的 `{amount:1,base:"USD",date,rates:{CNY:…}}` 原始缓存。v1 已标为弃用，官方说明仍继续可用；本项目新请求统一走 v2。旧格式的 `amount` 必须精确为数值 1，不能把 100 美元的换算结果当成 1 美元汇率。混合两种格式的响应不接受。

## 应用层接入约定

- 手动 USD/CNY 大于零时优先使用手动汇率；自动参考价用于没有手动值的情况。金额仍以 USD 存储，人民币显示才换算，不修改历史 USD 费用或 Token 数量。
- 自动联网只请求这个固定 HTTPS 公共汇率地址，不上传用量、模型、对话或凭据；它独立于模型价目更新。启动先读有效原始 JSON 缓存，再按应用的每日刷新策略异步下载，避免阻塞窗口。
- 新响应完整校验成功后，才以临时文件替换原始 JSON 缓存并更新有效引用；失败保留上次有效值及其原始日期。缓存过期时明确展示已有参考日期；既无有效缓存又联网失败时保持人民币换算未知，不能静默回退到硬编码的 7.2。
- 下载层负责 HTTPS、状态码、超时、正文大小上限和原子写入；不得执行响应内容。原始 JSON 可以直接交给 `ParseExchangeRateJson`，不需另存未经校验的派生费率。

`tests/exchange_tests.cpp` 包含实际 v1/v2 响应、合法闰年、费率正负与非有限值、币种与金额身份、缺失及重复字段、坏日期、混合格式、超大与多段响应验证。
