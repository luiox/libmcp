#pragma once

#include <optional>
#include <string>
#include <variant>

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

/// @brief JSON-RPC request id 的拥有型表示。
class JsonRpcId
{
public:
    /// @brief 从 UTF-8 字符串创建 id。
    static McpResult<JsonRpcId> from_string(std::string value);

    /// @brief 从整数创建 id。
    static JsonRpcId from_integer(ca::i64 value) noexcept;

    /// @brief 从有限浮点数创建 id。
    static McpResult<JsonRpcId> from_number(ca::f64 value);

    /// @brief 判断 id 是否为字符串。
    bool is_string() const noexcept;

    /// @brief 判断 id 是否为整数。
    bool is_integer() const noexcept;

    /// @brief 判断 id 是否为非整数 number。
    bool is_number() const noexcept;

    /// @brief 返回字符串值；其它类型返回 nullptr。
    const std::string* string_value() const noexcept;

    /// @brief 返回整数值；其它类型返回 nullptr。
    const ca::i64* integer_value() const noexcept;

    /// @brief 返回浮点值；其它类型返回 nullptr。
    const ca::f64* number_value() const noexcept;

private:
    using Storage = std::variant<std::string, ca::i64, ca::f64>;

    explicit JsonRpcId(Storage value) noexcept;

    Storage value_;

    friend class JsonRpcMessage;
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

    /// @brief 创建不带 params 的 request。
    static McpResult<JsonRpcMessage> make_request(JsonRpcId id, std::string method);

    /// @brief 创建带 object params 的 request，并接管 params document。
    static McpResult<JsonRpcMessage> make_request(JsonRpcId id, std::string method,
                                                  ca::json::JsonDocument params);

    /// @brief 创建不带 params 的 notification。
    static McpResult<JsonRpcMessage> make_notification(std::string method);

    /// @brief 创建带 object params 的 notification，并接管 params document。
    static McpResult<JsonRpcMessage> make_notification(std::string            method,
                                                       ca::json::JsonDocument params);

    /// @brief 创建 result response，并接管 object result document。
    static McpResult<JsonRpcMessage> make_result(JsonRpcId id, ca::json::JsonDocument result);

    /// @brief 创建不带 data 的 error response；id 为空时省略 id member。
    static McpResult<JsonRpcMessage> make_error(std::optional<JsonRpcId> id, ca::i64 code,
                                                std::string message);

    /// @brief 返回 message 形态。
    JsonRpcMessageKind kind() const noexcept;

    /// @brief 返回完整 JSON document。
    const ca::json::JsonDocument& document() const noexcept;

    /// @brief 返回 document root object。
    const ca::json::JsonValue& root() const noexcept;

    /// @brief 返回 request/response id；notification 或无 id error response 返回 nullptr。
    const ca::json::JsonValue* id() const noexcept;

    /// @brief 复制 request/response id；notification 或无 id error response 返回空 optional。
    std::optional<JsonRpcId> copy_id() const;

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
