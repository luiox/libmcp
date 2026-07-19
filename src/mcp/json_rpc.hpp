#pragma once

#include <optional>

#include <libca/json/json.hpp>
#include <libca/str/utf8_string.hpp>

#include "mcp/error.hpp"

namespace mcp {

/// @brief MCP 允许的四种 JSON-RPC message 形态。
enum class JsonRpcMessageKind
{
    Request,          ///< 包含非 null id 的 method 调用。
    Notification,     ///< 不包含 id 的 method 通知。
    ResultResponse,   ///< 包含 result 的成功响应。
    ErrorResponse     ///< 包含 error 的失败响应。
};

/// @brief 拥有一个通过 MCP JSON-RPC 基础约束校验的 JSON document。
/// @details 字符串与子节点引用都绑定到本对象持有的 JsonDocument；对象移动后，原对象上的
/// 引用全部失效。本类非线程安全。
class JsonRpcMessage
{
public:
    JsonRpcMessage(const JsonRpcMessage&)                = delete;
    JsonRpcMessage& operator=(const JsonRpcMessage&)     = delete;
    JsonRpcMessage(JsonRpcMessage&&) noexcept            = default;
    JsonRpcMessage& operator=(JsonRpcMessage&&) noexcept = default;
    ~JsonRpcMessage()                                    = default;

    /// @brief 解析 UTF-8 JSON 并校验单条 MCP JSON-RPC message。
    /// @note JSON batch、null id 和非 object params/result 会被拒绝。
    static McpResult<JsonRpcMessage> parse(const ca::str::Utf8StringRef& input);

    /// @brief 接管 JsonDocument 并校验其 root。
    static McpResult<JsonRpcMessage> from_document(ca::json::JsonDocument document);

    /// @brief 返回 message 形态。
    JsonRpcMessageKind kind() const noexcept;

    /// @brief 返回完整 JSON document。
    const ca::json::JsonDocument& document() const noexcept;

    /// @brief 返回 document root object。
    const ca::json::JsonValue& root() const noexcept;

    /// @brief 返回 request/response id；notification 或无 id error response 返回 nullptr。
    const ca::json::JsonValue* id() const noexcept;

    /// @brief 返回 request/notification method；response 返回空 optional。
    std::optional<ca::str::Utf8StringRef> method() const noexcept;

    /// @brief 返回 params；未提供时返回 nullptr。
    const ca::json::JsonValue* params() const noexcept;

    /// @brief 返回成功响应的 result；其它形态返回 nullptr。
    const ca::json::JsonValue* result() const noexcept;

    /// @brief 返回失败响应的 error object；其它形态返回 nullptr。
    const ca::json::JsonValue* error() const noexcept;

    /// @brief 紧凑序列化为单条 UTF-8 JSON；不追加 transport delimiter。
    ca::str::Utf8String serialize() const;

private:
    JsonRpcMessage(ca::json::JsonDocument document, JsonRpcMessageKind kind) noexcept;

    ca::json::JsonDocument document_;
    JsonRpcMessageKind     kind_{JsonRpcMessageKind::Notification};
};

}   // namespace mcp
