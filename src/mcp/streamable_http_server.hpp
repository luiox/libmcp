#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <libca/http/server.hpp>

#include "mcp/server.hpp"

namespace mcp {

/// @brief 为每个 Streamable HTTP session 创建独立 ServerSession 的回调。
/// @note 不同连接上的 initialize 可并发调用，factory 必须自行同步共享状态。
using HttpSessionFactory = std::function<McpResult<ServerSession>()>;

/// @brief buffered Streamable HTTP 服务端的 endpoint、安全与 session 限制。
struct StreamableHttpServerOptions
{
    std::string endpoint{"/mcp"};   ///< 单一 MCP origin-form endpoint。
    std::vector<std::string> allowed_origins;   ///< 允许的 Origin 精确值；空列表拒绝全部 Origin。
    ca::usize max_sessions{1024};   ///< 同时保留的 session 上限。
    ca::usize session_id_bytes{32};   ///< 安全随机 session id 的原始字节数，范围 16-64。
    bool require_protocol_version_header{true};   ///< 后续请求是否强制携带协商版本。
};

/// @brief 将独立 ServerSession 安装到 libca HttpServer 的 buffered Streamable HTTP adapter。
/// @details 当前支持 POST JSON、DELETE session 与 Origin 校验；GET 返回 405，不提供 SSE、
/// resumability 或 server-initiated request。不同 HTTP 连接可并发处理，不同 session 相互独立，
/// 同一 session 内的消息按 handler 获得锁的顺序串行执行。
class StreamableHttpServer
{
public:
    StreamableHttpServer(const StreamableHttpServer&)                = delete;
    StreamableHttpServer& operator=(const StreamableHttpServer&)     = delete;
    StreamableHttpServer(StreamableHttpServer&&) noexcept            = default;
    StreamableHttpServer& operator=(StreamableHttpServer&&) noexcept = default;
    ~StreamableHttpServer()                                          = default;

    /// @brief 校验配置并创建尚未安装 route 的 adapter。
    static McpResult<StreamableHttpServer> create(
        HttpSessionFactory          factory,
        StreamableHttpServerOptions options = StreamableHttpServerOptions());

    /// @brief 将 POST、GET 与 DELETE route 安装到 server。
    /// @note 必须在 HttpServer::serve() 前调用；middleware 与 route 会保持 adapter 状态生命周期。
    /// @warning HttpServer 不支持事务式 middleware/route 注册；失败后应丢弃可能已部分安装的
    /// server。
    ca::http::HttpResult<void> install(ca::http::HttpServer& server);

    /// @brief 线程安全地返回当前有效 session 数量。
    ca::usize session_count() const noexcept;

private:
    class Impl;

    explicit StreamableHttpServer(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> impl_;
};

}   // namespace mcp
