# libmcp

`libmcp` 是建立在 libca JSON、IO、net/http 之上的 C++17 MCP（Model Context
Protocol，JSON-RPC 2.0）基础库。自 [morpher](https://github.com/luiox) 单体仓
`libs/libmcp` 拆分而来（git subtree split 保留完整提交历史）。

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

## 状态与免责声明

- **代码由 AI 生成**，优先服务于作者个人项目；pre-1.0 阶段**不提供任何 API 兼容性 / 稳定性 / 可用性保证**，任何版本都可能引入破坏性变更。
- 本库按「现状」提供，是否用于生产环境请自行评估（完整免责条款见 [LICENSE](LICENSE)）。
- Issue 欢迎提，但**不承诺任何响应时效**：作者看到后会安排 AI 统一分诊处理，可能很快也可能长期搁置；feature 请求是否接受以作者自身项目需求为准。

## 构建（本仓库自构建）

依赖 [libca](https://github.com/luiox/libca)（经 [luiox-repo](https://github.com/luiox/luiox-repo)
包定义仓拉取，首次配置自动克隆）与 xmake ≥ 2.8.3：

```sh
xmake f -p windows -a x64 --with_tests=y -y   # 配置（带单测）
xmake -y                                       # 构建
xmake run -y libmcp_unittest                   # 运行全部单测
```

不配置 `--with_tests=y` 时只构建 `libmcp` 静态库本体。

## 作为包消费

```lua
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libmcp 0.0.2")

target("app")
    set_kind("binary")
    add_packages("libmcp")
    ...
```

设计文档与能力清单见 `doc_ai/`。
