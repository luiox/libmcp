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

- dual-era 单实例双语义：`server/discover` 免版本直接应答；`initialize` 进入 legacy lifecycle
  状态机；其余 request 按 `params._meta` 是否携带 modern（2026-07-28）版本选择无状态路径或
  legacy 会话路径（后者要求处于 Ready）。
- legacy：执行 initialize、initialized notification 与 ready 状态迁移；按客户端请求选择支持的
  协议版本，声明 server identity 与基础 capabilities；在所有 lifecycle 阶段内建处理 ping；
  ready 后把 request 分发给同步 method handler，并生成标准 method not found、invalid params
  和 internal error response。
- modern：不触碰 lifecycle 状态、可并发；请求版本不获支持时返回
  UnsupportedProtocolVersionError（`-32022`，data 带 supported/requested）。
- stdio server loop 把可恢复的 JSON/JSON-RPC 单行错误转换为协议 error response。
- handler 结果超过 stdio message 上限时，server loop 使用原 request id 返回精简 `-32603`
  JSON-RPC error，并继续处理后续消息；仅当精简错误也无法写出或底层 IO 失败时终止会话。

## modern era（2026-07-28）

- 无状态分派：每请求以 `params._meta["io.modelcontextprotocol/protocolVersion"]` 声明协议
  版本，不经 initialize/session，不改变 lifecycle 状态。
- `server/discover` 探测：免版本校验直接应答，返回 supportedVersions 与 server
  identity/capabilities。
- 错误形状：版本不获支持返回 `-32022`（data 携带 supported/requested）；Streamable HTTP 上
  `MCP-Protocol-Version` 头与 `_meta` 版本同时存在且不一致时返回 `-32020`（HTTP 层构造，
  id 取请求 id）。
- resultType 信封：modern 结果携带 `resultType`（普通完成为 `complete`）；legacy 结果永不
  添加。
- tools/list 确定性顺序，并在 `_meta` 附带 `ttlMs` 缓存新鲜度提示与 cacheScope（默认
  private，可配置 public）。
- HTTP 双模路由：无 `MCP-Session-Id` 的非 initialize POST 走共享 ServerSession 实例
  （注册先行即可并发），不创建 session、不占 session 上限、不返回 `MCP-Session-Id`；
  仅携带 `MCP-Protocol-Version` 头的请求由 HTTP 层把版本写入 `_meta` 后转交。

## tools

- `ToolDefinition` 拥有完整 Tool descriptor，保留官方字段与扩展字段。
- 校验 name、inputSchema 及已知可选字段的基础类型，不在 registry 内实现 JSON Schema 引擎。
- `ToolRegistry` 提供 tools/list 和 tools/call，并把未知工具与非法 arguments 映射为
  Invalid params。
- 安装 registry 时自动声明静态 tools capability，并冻结目录以兑现 `listChanged=false`。
- handler 的 CallToolResult 至少校验 content array、structuredContent object 与 isError bool。

## Streamable HTTP

- 在单一 endpoint 上接收一条 JSON-RPC message 的 POST；request 可返回 buffered
  `application/json`，也可配置为带 event id 的 `text/event-stream`；notification 与 client
  response 返回 HTTP 202。
- initialize 为每个 client 创建独立 `ServerSession`，使用系统安全随机数生成 session id，
  后续请求校验 `MCP-Session-Id` 与协商的 `MCP-Protocol-Version`。
- 对同一 session 串行执行 message，不同 session 可由 libca HTTP worker 并发处理。
- 对 MCP endpoint 的全部 method 在 route 前精确校验 Origin allowlist，再校验 JSON Content-Type
  与 JSON/SSE Accept 声明，并限制 session 总数。
- 可选 authorizer 为 POST/GET/DELETE 返回自定义拒绝响应或稳定主体 identity；identity 会传给
  session factory 并绑定 session，阻止其它主体复用 session id。
- DELETE 终止 session；不带 replay cursor 的普通 GET 返回 405，明确当前未开放独立 SSE
  stream。
- SSE response 先发送空 data priming event，再发送最终 JSON-RPC response，并按 session 同时
  限制 replay stream 数与编码字节数。
- GET 携带 `Last-Event-ID` 时只重放该 event 所属 stream 的后续事件；未知或已淘汰 cursor
  返回 404，普通 GET 仍返回 405。
- dual-era 双模路由：无 `MCP-Session-Id` 的非 initialize POST 走 modern 无状态路径
  （见 modern era 一节）；Origin allowlist、JSON Content-Type 与 JSON/SSE Accept 校验
  先于路由执行，对 modern 路径同样生效；modern 响应恒为 buffered `application/json`，
  不走 SSE。

## 尚未实现

- resource/prompt registry 与 dispatcher。
- subscriptions/listen 与 resource 更新订阅。
- MRTR 仅 resultType 信封透传，无 inputRequests 语义层。
- `Mcp-Method`/`Mcp-Name` 请求头未强制校验。
- Streamable HTTP 独立 GET stream 与 session expiry（legacy）。
- HTTPS 与 proxy-aware public origin policy。
- client capability 的持久化与 server-initiated request。
