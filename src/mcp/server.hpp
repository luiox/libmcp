#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <libca/json/json.hpp>

#include "mcp/json_rpc.hpp"
#include "mcp/stdio_transport.hpp"

namespace mcp {

class ToolRegistry;

// ── MCP 2026-07-28（modern era）协议常量 ─────────────────────

/// @brief modern 请求在 `_meta` 内声明协议版本的键名。
inline constexpr const char* kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
/// @brief modern 请求在 `_meta` 内声明客户端能力的键名。
inline constexpr const char* kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
/// @brief modern 请求在 `_meta` 内自报客户端身份的键名。
inline constexpr const char* kMetaClientInfo = "io.modelcontextprotocol/clientInfo";
/// @brief modern 结果在 `_meta` 内声明服务端身份的键名。
inline constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

/// @brief modern 结果必须携带的 resultType 取值（普通完成）。
inline constexpr const char* kResultTypeComplete = "complete";

/// @brief JSON-RPC 扩展错误码（2026-07-28 规范保留区 -32020..-32099）。
inline constexpr ca::i64 kHeaderMismatchError                  = -32020;
inline constexpr ca::i64 kMissingRequiredClientCapabilityError = -32021;
inline constexpr ca::i64 kUnsupportedProtocolVersionError      = -32022;

/// @brief Streamable HTTP 上与 `_meta` 并行携带协议版本的头。
inline constexpr const char* kProtocolVersionHeader = "MCP-Protocol-Version";

/// @brief JSON-RPC method handler 返回的协议错误。
class MethodError
{
public:
    /// @brief 创建任意 JSON-RPC error code 与 UTF-8 诊断文本。
    static MethodError from_code(ca::i64 code, std::string message);

    /// @brief 创建 Invalid params (-32602) 错误。
    static MethodError invalid_params(std::string message);

    /// @brief 创建 Internal error (-32603) 错误。
    static MethodError internal(std::string message);

    /// @brief 返回 JSON-RPC error code。
    ca::i64 code() const noexcept;

    /// @brief 返回错误诊断文本。
    const std::string& message() const noexcept;

private:
    MethodError(ca::i64 code, std::string message) noexcept;

    ca::i64     code_{-32603};
    std::string message_;
};

/// @brief method handler 的 object result 或 JSON-RPC error。
using MethodResult = ca::core::Result<ca::json::JsonDocument, MethodError>;

/// @brief 同步 JSON-RPC request handler；request 仅在回调期间有效。
using MethodHandler = std::function<MethodResult(const JsonRpcMessage& request)>;

/// @brief MCP 服务端生命周期状态。
enum class ServerSessionState
{
    AwaitingInitialize,    ///< 尚未成功处理 initialize request。
    AwaitingInitialized,   ///< 已回复 initialize，等待 initialized notification。
    Ready                  ///< capability negotiation 已完成，可分发普通 request。
};

/// @brief 服务端声明的基础 capabilities。
struct ServerCapabilities
{
    bool logging{false};                  ///< 声明 logging capability。
    bool completions{false};              ///< 声明 completions capability。
    bool prompts{false};                  ///< 声明 prompts capability。
    bool prompts_list_changed{false};     ///< prompts listChanged 子能力。
    bool resources{false};                ///< 声明 resources capability。
    bool resources_subscribe{false};      ///< resources subscribe 子能力。
    bool resources_list_changed{false};   ///< resources listChanged 子能力。
    bool tools{false};                    ///< 声明 tools capability。
    bool tools_list_changed{false};       ///< tools listChanged 子能力。
};

/// @brief MCP 服务端 identity、版本与 capability 配置。
struct ServerOptions
{
    std::string name;           ///< 必填的实现名称。
    std::string version;        ///< 必填的实现版本。
    std::string title;          ///< 可选的人类可读标题。
    std::string description;    ///< 可选的实现描述。
    std::string instructions;   ///< 可选的模型使用说明。
    /// @brief legacy（initialize 握手）协议版本池；首项作为不匹配时的首选版本。
    /// 允许为空，表示不服务 legacy 客户端（纯 modern 服务器）。
    std::vector<std::string> legacy_protocol_versions{"2025-11-25", "2025-03-26"};
    /// @brief modern（2026-07-28 无状态 per-request `_meta`）协议版本池，必须非空。
    std::vector<std::string> modern_protocol_versions{"2026-07-28"};
    ServerCapabilities       capabilities;   ///< initialize/discover 声明的能力。
    /// @brief modern 列表结果（tools/list、server/discover）携带的缓存新鲜度提示
    /// （毫秒）；负值表示不携带。legacy 结果永不携带。
    ca::i64     cache_ttl_ms{300000};
    /// @brief 缓存范围："private"（默认）或 "public"。
    std::string cache_scope{"private"};
    /// @brief modern 结果的 `_meta` 是否附带 serverInfo（规范 SHOULD）。
    bool        attach_server_info{true};
};

/// @brief 单个 MCP client connection 的同步服务端会话。
/// @details 双时代（dual-era）服务器：带 `_meta` 协议版本的 modern 请求按
/// 2026-07-28 无状态语义服务（不触碰 lifecycle 状态，可并发）；`initialize`
/// 请求选择 legacy 语义并进入 lifecycle 状态机（单线程使用）。`server/discover`
/// 按规范免版本校验直接应答。本类非线程安全，除非所有注册在并发服务前完成
/// 且 modern 与 legacy 路径不共享可变状态；一个 stdio connection 应使用一个
/// 独立实例，HTTP 部署可让 modern 请求共享同一实例（注册先行）。
class ServerSession
{
public:
    ServerSession(const ServerSession&)                = delete;
    ServerSession& operator=(const ServerSession&)     = delete;
    ServerSession(ServerSession&&) noexcept            = default;
    ServerSession& operator=(ServerSession&&) noexcept = default;
    ~ServerSession()                                   = default;

    /// @brief 校验配置并创建尚未初始化的会话。
    static McpResult<ServerSession> create(ServerOptions options);

    /// @brief 注册 ready 阶段的同步 request handler；重复或内建 method 返回错误。
    McpResult<void> register_method(std::string method, MethodHandler handler);

    /// @brief 安装 tools/list 与 tools/call registry，并自动声明 tools capability。
    /// @note 必须在 initialize 前调用；session 通过 shared_ptr 保持 registry 生命周期。
    McpResult<void> install_tools(std::shared_ptr<ToolRegistry> registry);

    /// @brief 处理一条已校验 message；notification 或被忽略的 response 返回空 optional。
    /// @details 分派规则：server/discover 恒走 modern 探测；initialize 走 legacy
    /// 协商；其余 request 按 `_meta` 是否携带 modern 版本选择无状态路径或
    /// legacy 会话路径（后者要求处于 Ready 状态）。modern 请求版本不获支持时
    /// 返回 UnsupportedProtocolVersionError（-32022，data 带 supported/requested）。
    McpResult<std::optional<JsonRpcMessage>> handle(const JsonRpcMessage& message);

    /// @brief 在 stdio transport 上运行至干净 EOF；可恢复的 JSON 错误会回复协议错误并继续。
    McpResult<void> serve_stdio(StdioTransport& transport);

    /// @brief 返回当前 lifecycle 状态（仅 legacy 路径使用；modern 请求不改变状态）。
    ServerSessionState state() const noexcept;

    /// @brief 返回 initialize response 选择的 legacy 协议版本；尚未初始化时为空字符串。
    const std::string& negotiated_protocol_version() const noexcept;

private:
    explicit ServerSession(ServerOptions options) noexcept;

    McpResult<std::optional<JsonRpcMessage>> handle_initialize(const JsonRpcMessage& message);
    McpResult<std::optional<JsonRpcMessage>> handle_modern_request(const JsonRpcMessage& message,
                                                                   const std::string&    method);
    McpResult<std::optional<JsonRpcMessage>> handle_request(const JsonRpcMessage& message,
                                                            const std::string&    method);
    McpResult<std::optional<JsonRpcMessage>> make_error_response(const JsonRpcMessage& request,
                                                                 ca::i64 code, std::string message);
    McpResult<std::optional<JsonRpcMessage>> make_empty_result(const JsonRpcMessage& request);
    ca::json::JsonDocument                   make_initialize_result(const std::string& version);

    ServerOptions                                  options_;
    ServerSessionState                             state_{ServerSessionState::AwaitingInitialize};
    std::string                                    negotiated_protocol_version_;
    std::unordered_map<std::string, MethodHandler> handlers_;
};

}   // namespace mcp
