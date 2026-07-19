# libmcp 设计

## 分层

```text
MCP features / lifecycle
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
