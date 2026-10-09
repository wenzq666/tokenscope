# 真实用量导入格式

软件列表中的所有来源都接受此规范格式。它是导入真实 Token 记录的入口；仅选择一个软件名称、安装了软件、持有聊天文本或积分记录，都不会生成用量。

在设置中选择“添加 / 导入来源”，选择记录所属的软件，再选择 JSON 文件、JSONL 文件或包含这些文件的文件夹。

## 规范 JSON

```json
{
  "schema": "tokenscope.usage.v1",
  "source": "manus",
  "records": [
    {
      "id": "actual-request-id-001",
      "timestamp": "2026-10-08T09:02:03+08:00",
      "model": "actual-model-id",
      "provider": "actual-provider",
      "usage": {
        "input_tokens": 100,
        "output_tokens": 20,
        "cached_input_tokens": 30,
        "cache_write_input_tokens": 5,
        "reasoning_output_tokens": 4,
        "total_tokens": 120
      }
    }
  ]
}
```

`source` 必须与所选来源 ID 一致，可以放在文档或每条记录上。`id` 必须来自真实请求、消息或执行记录；复制或更名导出文件不会改变请求身份。`timestamp` 必须为真实记录时间，带 `Z` 或明确时区偏移；不使用文件修改时间或导入时间推测历史日期。模型和提供商可以省略，省略后显示为未知。

输入与输出必须同时提供。规范的 `input_tokens` 已包含缓存读取和缓存写入；缓存两项是输入中的不同子集。`reasoning_output_tokens` 是输出的子集。总量不得小于输入与输出之和；较大的真实总量中未说明的剩余部分保留为“未分类”。缺少所有 Token 字段的记录无效；明确记录的零用量有效。

可选 `cache_write_1h_input_tokens` 表示总 `cache_write_input_tokens` 中明确为 1 小时缓存写入的部分，不能超过总写入量，也不再次加入总 Token。其余写入使用普通 / 5 分钟费率；没有真实的 1 小时计数时不要据软件名称猜测。模型价格和分桶不足时保留未定价，详见 [计费口径](pricing-reference.md)。

只有总量时可以这样写，软件不会猜测输入与输出，也不会据此估算费用：

```json
{
  "schema": "tokenscope.usage.v1",
  "source": "manus",
  "id": "actual-task-id-002",
  "timestamp": "2026-10-08T09:03:00+08:00",
  "usage": { "total_tokens": 123 }
}
```

JSONL 每一行均为上述完整单条对象，包含自己的 `schema` 和 `source`，并以换行符结束。正在写入、尚未换行的末尾记录会等待下次扫描。支持 UTF-8 BOM。

## 可直接导入的实际 API 用量

除规范格式外，还支持以下明确的响应结构：

- OpenAI Chat Completions：`object=chat.completion` 或 `chat.completion.chunk`，真实 `id`、Unix 秒 `created` 与 `usage`。
- OpenAI Responses：`object=response`，真实 `id`、Unix 秒 `created_at` 与 `usage`。
- Anthropic Messages：`type=message`、`role=assistant`、`id`、`usage`，并由外层提供真实带时区 `timestamp`。其输入中的普通 Token、缓存读取、缓存创建按官方定义相加一次。
- Dify：`message_end.metadata.usage` 加真实消息身份与时间；`workflow_finished.data` 的 `id`、`created_at` 与 `total_tokens`。共享明确 `task_id` / `workflow_run_id` 的消息与完整工作流记录，优先完整工作流，避免父子重复。这些关联会持久保存，分次导入和重启后也有效；原始子记录保留，但不重复进入汇总。若父记录只有总量，模型和输入/输出分项保持未知。
- Coze：工作流 history 的 `execute_id`、Unix 秒 `create_time`，优先当前 `usage.input_count / output_count / token_count`；旧版 `token` 字符串总量也可读取。

可以在明确的 `response`、`data`、`records`、`responses`、`message` 包装内提供响应。已经识别到父记录用量后不再深入子记录；聊天正文、`output`、凭据、任意执行节点树不参与 Token 搜索。n8n 等复杂执行导出需先提取实际模型响应，或转成上述规范格式。

JSON 文档上限 64 MiB，JSONL 文件上限 512 MiB，单行上限 16 MiB。文件夹扫描不跟随目录链接，只读取 JSON / JSONL。

字段定义参考官方来源：[OpenAI Responses](https://developers.openai.com/api/reference/resources/responses/methods/retrieve)、[Anthropic Messages](https://platform.claude.com/docs/en/api/messages/create)、[Dify API 事件定义](https://github.com/langgenius/dify/blob/main/api/core/app/entities/task_entities.py)、[Coze 工作流执行历史](https://docs.coze.cn/developer_guides_workflow_history)。
