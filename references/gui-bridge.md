# GUI 桥接：stream-json 协议与实现

`assets/gui` 是一个零前端构建的本地界面：Node HTTP 服务 + SSE 推送 + 每个会话一个常驻 Claude 子进程。

## 启动参数

```text
scc.exe -p
  --input-format stream-json
  --output-format stream-json
  --verbose
  --include-partial-messages
```

恢复历史会话时追加：

```text
--resume <claude_session_id>
```

说明：

- `-p`（print）是使用 stream-json 输入输出的前提。
- `--include-partial-messages` 才会输出 `stream_event` 增量；否则只能拿到完整 assistant 消息。
- `--verbose` 让事件更完整。
- 通过 `scc.exe` 启动可以复用便携环境；也可以直接启动 `claude.exe`，但需要自行复刻环境变量。

## 输入格式

每行一个 JSON（NDJSON），写入子进程 stdin：

```json
{"type":"user","message":{"role":"user","content":[{"type":"text","text":"你好"}]}}
```

stdin 保持打开时，同一进程可以连续多轮对话，上下文保留；关闭 stdin 后进程退出。

## 输出事件

每行一个 JSON。实测 Claude Code 2.1.x 的主要类型：

| 事件 | 关键字段 | 用途 |
|---|---|---|
| `system/init` | `session_id`、`cwd`、`model`、`permissionMode`、`tools`、`skills`、`slash_commands` | 建立会话、拿到 resume id |
| `system/status` | `status` | 显示"思考中/请求中" |
| `stream_event/message_start` | `event.message.id` | 开始一条 assistant 消息 |
| `stream_event/content_block_start` | `index`、`content_block.type` | 创建 text/thinking/tool_use 块 |
| `stream_event/content_block_delta` | `delta.type`：`text_delta` / `thinking_delta` / `input_json_delta` | 流式追加 |
| `stream_event/content_block_stop` | `index` | 结束块 |
| `assistant` | `message.id`、`message.content[]` | 完整消息，可用来校准流式内容 |
| `user` | `message.content[].type === "tool_result"` | 工具执行结果 |
| `result` | `subtype`、`total_cost_usd`、`duration_ms`、`usage` | 一轮结束 |
| `control_response` | `response.subtype` | 中断等控制请求的回应 |

注意：

- 工具结果不在 `assistant` 事件里，而是在随后的 `user` 事件中。
- 一条用户输入可能产生多条 `assistant` 消息（工具调用前后各一条），用 `message.id` 区分。
- `init` 每轮都会重新发送一次，前端处理要幂等。

## 中断

发送控制请求：

```json
{"type":"control_request","request_id":"req_1","request":{"subtype":"interrupt"}}
```

成功时返回：

```json
{"type":"control_response","response":{"subtype":"success","request_id":"req_1","response":{"still_queued":[]}}}
```

随后收到 `result`，`subtype` 为 `error_during_execution`。进程不会退出，可以继续发下一条消息。

实现建议：

- 中断后 5 秒仍没有 `result`，再强制结束进程树。
- Windows 上用 `taskkill /PID <pid> /T /F` 结束 `scc.exe` 及其子进程，避免孤儿进程。
- 前端不要因为 `error_during_execution` 就清空会话，它表示"被中断"，不是致命错误。

## 服务端架构

```text
Browser
  ├─ GET  /api/events        SSE，广播所有会话事件
  ├─ POST /api/chat          把消息写入对应 worker 的 stdin
  ├─ POST /api/interrupt     发送 control_request
  └─ GET  /api/sessions...   会话元数据与历史

Node server
  └─ Map<sessionId, worker>
       worker = { proc, buffer, busy, currentMessageId, blocksByMessage }
```

关键点：

- 每个会话一个常驻 worker，按需启动；限制并发数（例如 4），空闲超时回收。
- 服务端把原始事件归一化成前端易用的 `block_start` / `block_delta` / `message_final` / `tool_result`。
- 会话历史由服务端自己持久化（`gui/data/sessions.json`），Claude 的 `--resume` 负责上下文；两者不要混用。
- 会话记录只存文本与工具摘要，不复制凭据。

## 前端要点

- 只用浏览器原生能力：`fetch` + `EventSource`，无需打包工具。
- Markdown 用 `marked`，渲染结果必须经过 `DOMPurify` 清洗。
- 流式文本按 60–100 ms 节流重渲染，避免每个 token 都触发 Markdown 解析。
- 工具卡片默认折叠，运行时显示"运行中"，结果到达后显示"完成/失败"。
- 用户消息由服务端广播 `user_message` 事件驱动，不要只依赖本地插入，否则刷新或重连后会丢。

## 安全

- 只监听 `127.0.0.1`，不要绑定 `0.0.0.0`。
- 校验 `Origin`，只接受 `http://127.0.0.1:<port>` 和 `http://localhost:<port>`，降低跨站请求风险。
- 会话删除只删 GUI 记录，不删 Claude 自己的 `projects` 会话文件。
- GUI 继承 `scc.exe` 的权限模式；若改为对外分发，必须重新设计权限审批，不能默认 bypass。

## 已验证的坑

| 现象 | 原因 | 修复 |
|---|---|---|
| 弹窗、停止按钮隐藏失效 | 作者样式 `display:flex` 覆盖 `[hidden]` 的默认 `display:none` | 全局加 `[hidden]{display:none!important}` |
| 首条消息后空状态不消失 | 只 append 消息，没有移除 `.empty` | 追加消息前清理空状态 |
| 用户气泡不出现 | SSE 处理器漏了 `user_message` | 显式处理并渲染 |
| 顶部标题仍是"新会话" | 服务端更新了标题，前端只刷新侧栏 | `session_updated` 时同步当前标题 |
| 中断后进程残留 | 只结束启动器，没有结束进程树 | `taskkill /T /F` |
