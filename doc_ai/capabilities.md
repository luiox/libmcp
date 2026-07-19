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

## 尚未实现

- tool/resource/prompt registry 与 dispatcher。
- Streamable HTTP、SSE、session 与 resumability。
- HTTP authorization hook 和 Origin 校验。
- client capability 的持久化与 server-initiated request。
