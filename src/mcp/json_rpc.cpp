#include "mcp/json_rpc.hpp"

#include <string>
#include <utility>

#include <libca/str/utf8_util.hpp>

namespace mcp {
namespace {

using ca::json::JsonValue;
using ca::str::Utf8StringRef;

Utf8StringRef key(const char* value) noexcept
{
    return Utf8StringRef::from_cstr(value);
}

const JsonValue* member(const JsonValue& object, const char* name) noexcept
{
    if (!object.is_object()) return nullptr;
    return object.find(key(name));
}

bool valid_id(const JsonValue* id) noexcept
{
    return id != nullptr && (id->is_number() || id->is_string());
}

McpResult<JsonRpcMessageKind> validate_message(const JsonValue& root)
{
    if (!root.is_object())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "MCP JSON-RPC message root must be an object"));

    const auto* version = member(root, "jsonrpc");
    if (version == nullptr || !version->is_string() || version->as_string() != "2.0")
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "MCP JSON-RPC message must declare jsonrpc 2.0"));

    const auto* id     = member(root, "id");
    const auto* method = member(root, "method");
    const auto* params = member(root, "params");
    const auto* result = member(root, "result");
    const auto* error  = member(root, "error");

    if (method != nullptr) {
        if (!method->is_string())
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                     "JSON-RPC method must be a string"));
        if (result != nullptr || error != nullptr)
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidMessage,
                "JSON-RPC request or notification cannot contain result or error"));
        if (params != nullptr && !params->is_object())
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                     "MCP JSON-RPC params must be an object"));
        if (id == nullptr) return ca::core::Ok(JsonRpcMessageKind::Notification);
        if (!valid_id(id))
            return ca::core::Err(
                McpError::from_kind(McpErrorKind::InvalidMessage,
                                    "MCP JSON-RPC request id must be a string or number"));
        return ca::core::Ok(JsonRpcMessageKind::Request);
    }

    if ((result == nullptr) == (error == nullptr))
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidMessage,
                                "JSON-RPC response must contain exactly one of result or error"));
    if (params != nullptr)
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "JSON-RPC response cannot contain params"));

    if (result != nullptr) {
        if (!valid_id(id))
            return ca::core::Err(
                McpError::from_kind(McpErrorKind::InvalidMessage,
                                    "MCP JSON-RPC result response id must be a string or number"));
        if (!result->is_object())
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                     "MCP JSON-RPC result must be an object"));
        return ca::core::Ok(JsonRpcMessageKind::ResultResponse);
    }

    if (id != nullptr && !valid_id(id))
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidMessage,
            "MCP JSON-RPC error response id must be absent, a string, or a number"));
    if (!error->is_object())
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidMessage, "JSON-RPC error must be an object"));

    const auto* code    = member(*error, "code");
    const auto* message = member(*error, "message");
    if (code == nullptr || !code->is_int() || message == nullptr || !message->is_string())
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidMessage,
                                "JSON-RPC error must contain an integer code and string message"));
    return ca::core::Ok(JsonRpcMessageKind::ErrorResponse);
}

std::string parse_error_message(const ca::json::ParseError& error)
{
    return "JSON parse failed at line " + std::to_string(error.location.line) + ", column " +
           std::to_string(error.location.column) + ": " + error.message.to_std_string();
}

}   // namespace

JsonRpcMessage::JsonRpcMessage(ca::json::JsonDocument document, JsonRpcMessageKind kind) noexcept
    : document_(std::move(document))
    , kind_(kind)
{}

McpResult<JsonRpcMessage> JsonRpcMessage::parse(const Utf8StringRef& input)
{
    if (!ca::str::utf8_is_valid(input.data(), input.byte_length()))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidJson,
                                                 "MCP JSON-RPC message is not valid UTF-8"));

    auto parsed = ca::json::JsonReader::read(input);
    if (parsed.is_err()) {
        auto error = std::move(parsed).unwrap_err();
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidJson, parse_error_message(error)));
    }
    return from_document(std::move(parsed).unwrap());
}

McpResult<JsonRpcMessage> JsonRpcMessage::from_document(ca::json::JsonDocument document)
{
    auto kind = validate_message(document.root());
    if (kind.is_err()) return ca::core::Err(std::move(kind).unwrap_err());
    return ca::core::Ok(JsonRpcMessage(std::move(document), kind.unwrap()));
}

JsonRpcMessageKind JsonRpcMessage::kind() const noexcept
{
    return kind_;
}

const ca::json::JsonDocument& JsonRpcMessage::document() const noexcept
{
    return document_;
}

const JsonValue& JsonRpcMessage::root() const noexcept
{
    return document_.root();
}

const JsonValue* JsonRpcMessage::id() const noexcept
{
    return member(document_.root(), "id");
}

std::optional<Utf8StringRef> JsonRpcMessage::method() const noexcept
{
    const auto* value = member(document_.root(), "method");
    if (value == nullptr || !value->is_string()) return std::nullopt;
    return value->as_string();
}

const JsonValue* JsonRpcMessage::params() const noexcept
{
    return member(document_.root(), "params");
}

const JsonValue* JsonRpcMessage::result() const noexcept
{
    return kind_ == JsonRpcMessageKind::ResultResponse ? member(document_.root(), "result")
                                                       : nullptr;
}

const JsonValue* JsonRpcMessage::error() const noexcept
{
    return kind_ == JsonRpcMessageKind::ErrorResponse ? member(document_.root(), "error") : nullptr;
}

ca::str::Utf8String JsonRpcMessage::serialize() const
{
    return ca::json::JsonWriter::write(document_);
}

}   // namespace mcp
