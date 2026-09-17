# libmcp [![CI](https://github.com/luiox/libmcp/actions/workflows/ci.yml/badge.svg)](https://github.com/luiox/libmcp/actions/workflows/ci.yml) [![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

C++17 的 MCP（Model Context Protocol）服务端基础库：stdio 与 Streamable HTTP 传输、工具注册与分发，构建于 [libca](https://github.com/luiox/libca) 之上。

## 特性

* 单条 MCP JSON-RPC 2.0 message 的严格解析与结构校验
* stdio transport（MCP 2025-11-25，newline-delimited），可运行至 EOF 并恢复单行 JSON 错误
* 服务端 initialize/initialized 生命周期、协议版本协商、ping 与同步 method 分发
* 静态 tool registry：完整 descriptor 校验、`tools/list`、`tools/call` 分发
* Streamable HTTP server：可选 SSE、Last-Event-ID 重放、Origin allowlist、authorization hook、并发 session

### dual-era 双协议

同一 Streamable HTTP endpoint 可并发服务 MCP 2026-07-28（modern）与 2025-11-25（legacy）
两代客户端：

* **modern 无状态路径**：不带 `MCP-Session-Id` 的非 initialize 请求按 2026-07-28 语义服务。
  版本由每请求 `params._meta["io.modelcontextprotocol/protocolVersion"]` 声明（HTTP 上可改为
  仅携带 `MCP-Protocol-Version` 头，两者同时存在且不一致时返回 `-32020`）。`server/discover`
  免版本直接应答；modern 结果携带 `resultType` 信封，列表结果附 `ttlMs` 缓存新鲜度提示。
  modern 请求不创建 session、不占 session 上限，响应恒为 buffered `application/json`。
* **legacy 完整保留**：`initialize` 仍创建 session 并返回 `MCP-Session-Id`，后续请求经
  session 串行处理，支持 SSE 重放、`MCP-Protocol-Version` 校验与 ping；legacy 结果不添加
  modern 信封字段。

`server/discover` 响应示例：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "resultType": "complete",
    "supportedVersions": ["2026-07-28"],
    "serverInfo": {"name": "my-server", "version": "1.0.0"},
    "capabilities": {"tools": {"listChanged": false}},
    "_meta": {"ttlMs": 300000}
  }
}
```

## 使用

### 接入

xmake ≥ 2.8.3，包定义来自 [luiox-repo](https://github.com/luiox/luiox-repo)：

```lua
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libmcp 0.1.0")

target("app")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("libmcp")
```

### 一个最小的 MCP server

```cpp
#include <mcp/mcp.hpp>

// 工具回调：入参为 JSON arguments，返回 MCP result document
static mcp::MethodResult echo(const ca::json::JsonValue& arguments) {
    ca::json::JsonDocument document;
    auto content_item = ca::json::JsonValue::make_object();
    content_item.set(document.arena().intern("type"),
                     ca::json::JsonValue::make_string(document.arena().intern("text")));
    content_item.set(document.arena().intern("text"),
                     ca::json::JsonValue::make_string(document.arena().intern("hello")));
    auto content = ca::json::JsonValue::make_array();
    content.append(std::move(content_item));
    auto result = ca::json::JsonValue::make_object();
    result.set(document.arena().intern("content"), std::move(content));
    result.set(document.arena().intern("isError"), ca::json::JsonValue::make_bool(false));
    document.root() = std::move(result);
    return ca::core::Ok(std::move(document));
}

int main() {
    mcp::ServerOptions options;
    options.name    = "my-server";
    options.version = "1.0.0";
    options.capabilities.tools = true;

    auto created = mcp::ServerSession::create(std::move(options));
    if (created.is_err()) return 1;
    auto server = std::move(created).unwrap();

    // 注册工具：definition 为标准 MCP tool JSON（name/description/inputSchema）
    auto registry = std::make_shared<mcp::ToolRegistry>();
    auto tool = mcp::ToolDefinition::parse(ca::str::Utf8StringRef::from_cstr(
        R"({"name":"echo","description":"Echo text","inputSchema":{"type":"object"}})"));
    registry->register_tool(std::move(tool).unwrap(), echo);
    server.install_tools(registry);

    // stdio 传输：借用宿主的 stdin/stdout 字节流（libca io 的 native stream），
    // serve 至 EOF；initialize / tools/list / tools/call 全部内部分发。
    mcp::StdioTransport transport =
        mcp::StdioTransport::create(reader, writer).unwrap();
    return server.serve_stdio(transport).is_ok() ? 0 : 1;
}
```

## 构建（本仓库开发）

```bash
xmake f -p windows -a x64 --with_tests=y -y   # Windows/MSVC
xmake f -p linux --with_tests=y -y            # Linux
xmake
xmake run libmcp_unittest
```

## 说明

* 本库**由 AI 生成**，优先服务于作者个人项目；pre-1.0 阶段无 API 兼容性与可用性保证，生产使用请自行评估（免责条款见 [LICENSE](LICENSE)）。
* Issue 欢迎提：作者会安排 AI 分诊处理，但不承诺时效。
* 设计文档与能力边界见 `doc_ai/`。

## License

[Apache-2.0](LICENSE) © Canrad
