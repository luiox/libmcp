#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <libca/json/json.hpp>

#include "mcp/json_rpc.hpp"
#include "mcp/stdio_transport.hpp"

namespace mcp {

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
    /// @brief 支持的协议版本，首项作为不匹配时返回的首选版本。
    std::vector<std::string> supported_protocol_versions{"2025-11-25"};
    ServerCapabilities       capabilities;   ///< initialize response 声明的能力。
};

/// @brief 单个 MCP client connection 的同步服务端会话。
/// @details 会话拥有 lifecycle 状态与 method registry，不拥有 transport。initialize、ping 和
/// notifications/initialized 由会话内建处理；其它 notification 当前按 JSON-RPC 规则忽略。
/// 本类非线程安全，一个 connection 应使用一个独立实例。
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

    /// @brief 处理一条已校验 message；notification 或被忽略的 response 返回空 optional。
    McpResult<std::optional<JsonRpcMessage>> handle(const JsonRpcMessage& message);

    /// @brief 在 stdio transport 上运行至干净 EOF；可恢复的 JSON 错误会回复协议错误并继续。
    McpResult<void> serve_stdio(StdioTransport& transport);

    /// @brief 返回当前 lifecycle 状态。
    ServerSessionState state() const noexcept;

    /// @brief 返回 initialize response 选择的协议版本；尚未初始化时为空字符串。
    const std::string& negotiated_protocol_version() const noexcept;

private:
    explicit ServerSession(ServerOptions options) noexcept;

    McpResult<std::optional<JsonRpcMessage>> handle_initialize(const JsonRpcMessage& message);
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
