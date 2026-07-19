# libmcp capabilities

## JSON-RPC

- 解析并拥有一条 UTF-8 MCP JSON-RPC message。
- 区分 request、notification、result response 与 error response。
- 拒绝 batch、非法 id、非 object params/result 和错误对象缺字段。
- 使用 libca `JsonDocument` 保证 DOM 字符串与节点引用生命周期。

## stdio

- 一行一条紧凑 JSON-RPC message，使用单个 LF 分隔。
- 支持碎片化 Reader、连续 message、短写 Writer 和显式 flush。
- 支持 message 字节上限、干净 EOF、截断 framing 与 IO 错误。

## 尚未实现

- MCP lifecycle 与 capability negotiation。
- tool/resource/prompt registry 与 dispatcher。
- Streamable HTTP、SSE、session 与 resumability。
- HTTP authorization hook 和 Origin 校验。
