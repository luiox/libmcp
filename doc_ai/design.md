# libmcp 设计

## 分层

```text
MCP feature registries
        |
ServerSession lifecycle + method dispatch
        |
JSON-RPC message ownership
        |
stdio transport | Streamable HTTP transport
        |
libca_json + libca_io + libca_http
```

JSON-RPC 层只负责 wire message 的 UTF-8、JSON 与基础结构约束，不认识 tools、resources
等业务 method。transport 只负责 message 边界和收发，不承担 dispatcher 语义。

`JsonRpcMessage` 持有整个 `JsonDocument`，避免把 arena 中的 `Utf8StringRef` 泄漏到所有权根
之外。stdio 按 MCP 2025-11-25 使用 newline framing；旧实现的 `Content-Length` framing
不属于当前 MCP stdio transport。

HTTP 层后续只使用单一 MCP endpoint，POST/GET/DELETE、SSE 和 session 语义放在 libmcp，
HTTP/1 framing、deadline、连接与路由仍由 libca_http 负责。

## server session

`ServerSession` 对应一个 client connection，状态严格按 `AwaitingInitialize`、
`AwaitingInitialized`、`Ready` 前进。initialize response 选择客户端请求的受支持版本；不匹配时
返回配置中的首选版本，由客户端决定是否断开。普通 request 只在 ready 阶段进入同步 method
handler，ping 是 lifecycle 各阶段均可用的内建 method。

handler 返回独立的 object `JsonDocument`，session 在同一 document arena 内包成 response，
避免跨 document 浅拷贝 `Utf8StringRef`。request id 先复制为拥有型 `JsonRpcId`，因此响应不依赖
输入 message 生命周期。handler 的业务失败使用 `MethodError` 生成 JSON-RPC error；transport、
framing 和本地状态错误仍使用 `McpError`。

当前 session 只校验 initialize 中 client capabilities 的 object 形态，不持久化该 DOM，也不发起
server-to-client request。后续引入 sampling/roots 等客户端能力时，应增加明确的拥有型 capability
快照，而不是保存指向 initialize request arena 的视图。
