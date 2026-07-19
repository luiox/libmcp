# libmcp

`libmcp` 是建立在 libca JSON、IO、net/http 之上的 C++17 MCP 基础库。

当前增量提供：

- 严格的单条 MCP JSON-RPC 2.0 message 解析与结构校验。
- request/notification/result/error 的拥有型出站构造。
- 拥有 `JsonDocument` 的 message 生命周期边界。
- 符合 MCP 2025-11-25 的 newline-delimited stdio transport。
- UTF-8、message 大小、EOF 和底层 IO 错误处理。
- 服务端 initialize/initialized/ready 生命周期、版本协商、ping 与同步 method 分发。
- 可直接运行至 EOF 且能恢复单行 JSON 错误的 stdio server loop。
- 拥有完整 descriptor 的静态 tool registry，以及 tools/list、tools/call 分发。
- 基于 libca HTTP/net 的 Streamable HTTP server、可选 SSE response 与 Last-Event-ID 重放、
  Origin allowlist、authorization hook 与并发 session 管理。

Streamable HTTP 的独立 GET stream、server-initiated request、HTTPS 和 resources/prompts
feature registry 将在后续增量加入。
MCP wire format 使用 JSON-RPC，不需要 XML。
