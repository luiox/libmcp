# libmcp capabilities

## JSON-RPC

- 解析并拥有一条 UTF-8 MCP JSON-RPC message。
- 区分 request、notification、result response 与 error response。
- 拒绝 batch、非法 id、非 object params/result 和错误对象缺字段。
- 使用 libca `JsonDocument` 保证 DOM 字符串与节点引用生命周期。
- 使用拥有型 `JsonRpcId` 构造 request、notification、result 与 error message。

## stdio

- 一行一条紧凑 JSON-RPC message，使用单个 LF 分隔。
- 支持碎片化 Reader、连续 message、短写 Writer 和显式 flush。
- 支持 message 字节上限、干净 EOF、截断 framing 与 IO 错误。

## server session

- 执行 initialize、initialized notification 与 ready 状态迁移。
- 按客户端请求选择支持的协议版本，声明 server identity 与基础 capabilities。
- 在所有 lifecycle 阶段内建处理 ping。
- ready 后把 request 分发给同步 method handler，并生成标准 method not found、invalid params
  和 internal error response。
- stdio server loop 把可恢复的 JSON/JSON-RPC 单行错误转换为协议 error response。

## tools

- `ToolDefinition` 拥有完整 Tool descriptor，保留官方字段与扩展字段。
- 校验 name、inputSchema 及已知可选字段的基础类型，不在 registry 内实现 JSON Schema 引擎。
- `ToolRegistry` 提供 tools/list 和 tools/call，并把未知工具与非法 arguments 映射为
  Invalid params。
- 安装 registry 时自动声明静态 tools capability，并冻结目录以兑现 `listChanged=false`。
- handler 的 CallToolResult 至少校验 content array、structuredContent object 与 isError bool。

## Streamable HTTP

- 在单一 endpoint 上接收一条 JSON-RPC message 的 POST，并返回 buffered `application/json`
  response；notification 与 client response 返回 HTTP 202。
- initialize 为每个 client 创建独立 `ServerSession`，使用系统安全随机数生成 session id，
  后续请求校验 `MCP-Session-Id` 与协商的 `MCP-Protocol-Version`。
- 对同一 session 串行执行 message，不同 session 可由 libca HTTP worker 并发处理。
- 对 MCP endpoint 的全部 method 在 route 前精确校验 Origin allowlist，再校验 JSON Content-Type
  与 JSON/SSE Accept 声明，并限制 session 总数。
- DELETE 终止 session；GET 返回 405，明确当前未开放独立 SSE stream。

## 尚未实现

- resource/prompt registry 与 dispatcher。
- Streamable HTTP SSE、resumability 与 session expiry。
- HTTP authorization hook、HTTPS 与 proxy-aware public origin policy。
- client capability 的持久化与 server-initiated request。
