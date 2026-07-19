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

HTTP 层使用单一 MCP endpoint。libmcp 负责 POST/GET/DELETE、Origin、协议版本和 session
语义，HTTP/1 framing、deadline、连接与路由仍由 libca_http 负责。首版 POST 每次完整缓冲一条
JSON-RPC message，GET 返回 405；SSE 与 resumability 后续在相同 adapter 内扩展，不下沉到
libca_http。

每次 initialize 通过 `HttpSessionFactory` 创建独立 `ServerSession`，安全随机 session id 只在
initialize 成功后发给 client。全局 map mutex 只保护 session 目录和容量预留，不在持锁时调用
factory；每个 session 的独立 mutex 保证来自多个 HTTP connection 的 message 串行进入非线程安全的
`ServerSession`。DELETE 先标记 session 失效再从目录移除，避免已取得共享引用但尚未进入 handler
的并发 POST 在删除完成后继续执行。
factory 或 session handler 的异常会转换为 HTTP 500；初始化失败释放容量预留，已有 session 的
不可恢复错误会先标记失效再从目录移除，避免后续请求继续进入可能损坏的状态。

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

## tools registry

`ToolDefinition` 持有完整 descriptor `JsonDocument`，而不是把不断演进的 Tool schema 展开成固定
C++ struct。tools/list 构建响应时递归复制 JSON value，并把所有 key/string 重新 intern 到响应
document arena，禁止跨 document 浅拷贝字符串引用。

`ServerSession::install_tools` 接受 `shared_ptr<ToolRegistry>`，两个 method handler 捕获该所有权，
不会依赖调用方栈对象寿命。安装后 registry 冻结并声明 `tools.listChanged=false`；动态目录及
notifications/tools/list_changed 必须与 server-to-client notification 支持一起加入，不能只开放
运行期写入。

tool handler 接收只在回调期有效的 arguments object，返回拥有型 CallToolResult document。
工具正常执行失败应使用 `isError=true`，只有协议参数错误与 handler 内部错误才使用
`MethodError` 生成 JSON-RPC error response。
