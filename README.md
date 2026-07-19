# libmcp

`libmcp` 是建立在 libca JSON、IO、net/http 之上的 C++17 MCP 基础库。

当前增量提供：

- 严格的单条 MCP JSON-RPC 2.0 message 解析与结构校验。
- 拥有 `JsonDocument` 的 message 生命周期边界。
- 符合 MCP 2025-11-25 的 newline-delimited stdio transport。
- UTF-8、message 大小、EOF 和底层 IO 错误处理。

HTTP Streamable transport、session、lifecycle 和 server feature registry 将在后续增量加入。
MCP wire format 使用 JSON-RPC，不需要 XML。
