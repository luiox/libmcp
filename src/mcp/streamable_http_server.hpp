#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libca/http/server.hpp>

#include "mcp/server.hpp"

namespace mcp {

/// @brief authorization hook 对一条 MCP HTTP request 的允许或拒绝决策。
/// @details authorized identity 用于绑定 session；rejection 可携带 401/403 与认证挑战 headers。
class HttpAuthorizationDecision
{
public:
    HttpAuthorizationDecision(const HttpAuthorizationDecision&)                = delete;
    HttpAuthorizationDecision& operator=(const HttpAuthorizationDecision&)     = delete;
    HttpAuthorizationDecision(HttpAuthorizationDecision&&) noexcept            = default;
    HttpAuthorizationDecision& operator=(HttpAuthorizationDecision&&) noexcept = default;
    ~HttpAuthorizationDecision()                                               = default;

    /// @brief 允许 request 并提供稳定的认证主体 identity。
    /// @note 配置 authorizer 时 identity 必须非空；同一主体的不同 request 必须返回相同值。
    static HttpAuthorizationDecision authorized(std::string identity);

    /// @brief 拒绝 request 并直接返回给 client。
    static HttpAuthorizationDecision rejected(ca::http::HttpServerResponse response);

    /// @brief 判断当前决策是否允许 request。
    bool is_authorized() const noexcept;

    /// @brief 返回认证主体 identity；拒绝决策返回空字符串。
    const std::string& identity() const noexcept;

    /// @brief 移出拒绝响应。
    /// @warning 只能对拒绝决策调用，且只能调用一次。
    ca::http::HttpServerResponse take_rejection();

private:
    explicit HttpAuthorizationDecision(std::string identity);
    explicit HttpAuthorizationDecision(ca::http::HttpServerResponse response);

    bool                                        authorized_{false};
    std::string                                 identity_;
    std::optional<ca::http::HttpServerResponse> rejection_;
};

/// @brief 对一条 MCP endpoint request 执行应用自定义 authorization 的回调。
/// @note POST、GET 与 DELETE 可由不同 worker 并发调用；实现必须自行同步共享状态。
using HttpRequestAuthorizer = std::function<ca::http::HttpResult<HttpAuthorizationDecision>(
    const ca::http::HttpServerRequestContext&)>;

/// @brief 创建 Streamable HTTP session 时有效的 request 与认证主体上下文。
/// @warning request 引用与 authorization_identity 只在 HttpSessionFactory 回调期间有效。
struct HttpSessionContext
{
    const ca::http::HttpServerRequestContext& request;   ///< initialize request 上下文。
    std::string_view authorization_identity;             ///< 未配置 authorizer 时为空。
};

/// @brief 为每个 Streamable HTTP session 创建独立 ServerSession 的回调。
/// @note 不同连接上的 initialize 可并发调用，factory 必须自行同步共享状态。
using HttpSessionFactory = std::function<McpResult<ServerSession>(const HttpSessionContext&)>;

/// @brief Streamable HTTP SSE response 与断线重放限制。
struct StreamableHttpSseOptions
{
    /// @brief 每个 session 最多保留的已终止 SSE stream 数量。
    ca::usize max_replay_streams{64};

    /// @brief 每个 session 最多保留的 SSE 编码字节数。
    /// @note 单个 stream 超过该限制时仍会发送，但不会进入 replay history。
    ca::usize max_replay_bytes{4 * 1024 * 1024};
};

/// @brief Streamable HTTP 服务端的 endpoint、安全、SSE 与 session 限制。
struct StreamableHttpServerOptions
{
    std::string endpoint{"/mcp"};   ///< 单一 MCP origin-form endpoint。
    std::vector<std::string> allowed_origins;   ///< 允许的 Origin 精确值；空列表拒绝全部 Origin。
    HttpRequestAuthorizer authorizer;   ///< 可选的每 request authorization；空回调表示禁用。
    /// @brief modern（2026-07-28 无状态请求）共享的 `ServerSession` 处理实例。
    /// @note 与 session factory 至少配置其一，可同时配置（dual-era）。modern 请求在该实例上
    /// 无状态处理：不创建 session、不占 `max_sessions` 名额、不返回 `MCP-Session-Id`。
    /// 全部 method/registry 注册必须在 HTTP 服务开始前完成。
    std::shared_ptr<ServerSession> shared_server;
    /// @brief 设置后使用带 event id 的 SSE 返回 JSON-RPC request response。
    /// @note HTTP/1.0 自动回退 application/json；GET 仅用于携带 Last-Event-ID 重放已终止
    /// stream，不开放独立消息 stream。modern 路径不使用 SSE，恒为 buffered application/json。
    std::optional<StreamableHttpSseOptions> sse;
    ca::usize max_sessions{1024};   ///< 同时保留的 legacy session 上限；modern 请求不占名额。
    ca::usize session_id_bytes{32};   ///< 安全随机 session id 的原始字节数，范围 16-64。
    bool require_protocol_version_header{true};   ///< legacy 会话请求是否强制携带协商版本。
};

/// @brief 将独立或共享 ServerSession 安装到 libca HttpServer 的 Streamable HTTP adapter。
/// @details dual-era 双模路由：带 `MCP-Session-Id` 的 POST/GET/DELETE 走 legacy 会话路径
/// （session 查找、同一 session 串行、SSE 重放、DELETE 终止）；无 session id 的 POST 中
/// `initialize` 仍创建 legacy session（老客户端入口），其余请求（含 server/discover）走
/// modern 无状态路径，交给 `shared_server` 处理。modern 路径不创建 session、不计入 session
/// 总数上限，且始终返回 buffered `application/json`、不返回 `MCP-Session-Id`。支持 buffered
/// 或 SSE POST response、Last-Event-ID 重放、DELETE session、Origin 与可选 authorization；
/// 不提供独立 GET stream 或 server-initiated request。不同 HTTP 连接可并发处理，不同 session
/// 相互独立，同一 session 内的消息按 handler 获得锁的顺序串行执行。
class StreamableHttpServer
{
public:
    StreamableHttpServer(const StreamableHttpServer&)                = delete;
    StreamableHttpServer& operator=(const StreamableHttpServer&)     = delete;
    StreamableHttpServer(StreamableHttpServer&&) noexcept            = default;
    StreamableHttpServer& operator=(StreamableHttpServer&&) noexcept = default;
    ~StreamableHttpServer()                                          = default;

    /// @brief 校验配置并创建尚未安装 route 的 adapter。
    /// @note session factory 与 `options.shared_server` 至少提供一个，两者可同时存在
    /// （dual-era）。
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
