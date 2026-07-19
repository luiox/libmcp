#include "mcp/json_rpc.hpp"

#include <cmath>
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

bool valid_utf8(const std::string& value) noexcept
{
    return ca::str::utf8_is_valid(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

JsonValue id_value(ca::json::JsonDocument& document, const JsonRpcId& id)
{
    if (const auto* value = id.string_value())
        return JsonValue::make_string(
            document.arena().intern(reinterpret_cast<const ca::u8*>(value->data()), value->size()));
    if (const auto* value = id.integer_value()) return JsonValue::make_int(*value);
    return JsonValue::make_float(*id.number_value());
}

McpResult<void> validate_method(const std::string& method)
{
    if (!valid_utf8(method))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "JSON-RPC method is not valid UTF-8"));
    return ca::core::Ok();
}

McpResult<JsonRpcMessage> make_call(JsonRpcId id, std::string method,
                                    ca::json::JsonDocument document, bool has_params,
                                    JsonRpcMessageKind kind)
{
    auto valid_method = validate_method(method);
    if (valid_method.is_err()) return ca::core::Err(std::move(valid_method).unwrap_err());
    if (has_params && !document.root().is_object())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "MCP JSON-RPC params must be an object"));

    JsonValue params;
    if (has_params) params = std::move(document.root());
    JsonValue root = JsonValue::make_object();
    root.set(document.arena().intern("jsonrpc"),
             JsonValue::make_string(document.arena().intern("2.0")));
    if (kind == JsonRpcMessageKind::Request)
        root.set(document.arena().intern("id"), id_value(document, id));
    root.set(document.arena().intern("method"),
             JsonValue::make_string(document.arena().intern(
                 reinterpret_cast<const ca::u8*>(method.data()), method.size())));
    if (has_params) root.set(document.arena().intern("params"), std::move(params));
    document.root() = std::move(root);
    return JsonRpcMessage::from_document(std::move(document));
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

JsonRpcId::JsonRpcId(Storage value) noexcept
    : value_(std::move(value))
{}

McpResult<JsonRpcId> JsonRpcId::from_string(std::string value)
{
    if (!valid_utf8(value))
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidMessage, "JSON-RPC id is not valid UTF-8"));
    return ca::core::Ok(JsonRpcId(Storage(std::move(value))));
}

JsonRpcId JsonRpcId::from_integer(ca::i64 value) noexcept
{
    return JsonRpcId(Storage(value));
}

McpResult<JsonRpcId> JsonRpcId::from_number(ca::f64 value)
{
    if (!std::isfinite(value))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "JSON-RPC numeric id must be finite"));
    return ca::core::Ok(JsonRpcId(Storage(value)));
}

bool JsonRpcId::is_string() const noexcept
{
    return std::holds_alternative<std::string>(value_);
}

bool JsonRpcId::is_integer() const noexcept
{
    return std::holds_alternative<ca::i64>(value_);
}

bool JsonRpcId::is_number() const noexcept
{
    return std::holds_alternative<ca::f64>(value_);
}

const std::string* JsonRpcId::string_value() const noexcept
{
    return std::get_if<std::string>(&value_);
}

const ca::i64* JsonRpcId::integer_value() const noexcept
{
    return std::get_if<ca::i64>(&value_);
}

const ca::f64* JsonRpcId::number_value() const noexcept
{
    return std::get_if<ca::f64>(&value_);
}

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

McpResult<JsonRpcMessage> JsonRpcMessage::make_request(JsonRpcId id, std::string method)
{
    return make_call(std::move(id),
                     std::move(method),
                     ca::json::JsonDocument(),
                     false,
                     JsonRpcMessageKind::Request);
}

McpResult<JsonRpcMessage> JsonRpcMessage::make_request(JsonRpcId id, std::string method,
                                                       ca::json::JsonDocument params)
{
    return make_call(
        std::move(id), std::move(method), std::move(params), true, JsonRpcMessageKind::Request);
}

McpResult<JsonRpcMessage> JsonRpcMessage::make_notification(std::string method)
{
    return make_call(JsonRpcId::from_integer(0),
                     std::move(method),
                     ca::json::JsonDocument(),
                     false,
                     JsonRpcMessageKind::Notification);
}

McpResult<JsonRpcMessage> JsonRpcMessage::make_notification(std::string            method,
                                                            ca::json::JsonDocument params)
{
    return make_call(JsonRpcId::from_integer(0),
                     std::move(method),
                     std::move(params),
                     true,
                     JsonRpcMessageKind::Notification);
}

McpResult<JsonRpcMessage> JsonRpcMessage::make_result(JsonRpcId id, ca::json::JsonDocument result)
{
    if (!result.root().is_object())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "MCP JSON-RPC result must be an object"));
    JsonValue result_root = std::move(result.root());
    JsonValue root        = JsonValue::make_object();
    root.set(result.arena().intern("jsonrpc"),
             JsonValue::make_string(result.arena().intern("2.0")));
    root.set(result.arena().intern("id"), id_value(result, id));
    root.set(result.arena().intern("result"), std::move(result_root));
    result.root() = std::move(root);
    return from_document(std::move(result));
}

McpResult<JsonRpcMessage> JsonRpcMessage::make_error(std::optional<JsonRpcId> id, ca::i64 code,
                                                     std::string message)
{
    if (!valid_utf8(message))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "JSON-RPC error message is not valid UTF-8"));
    ca::json::JsonDocument document;
    JsonValue              error = JsonValue::make_object();
    error.set(document.arena().intern("code"), JsonValue::make_int(code));
    error.set(document.arena().intern("message"),
              JsonValue::make_string(document.arena().intern(
                  reinterpret_cast<const ca::u8*>(message.data()), message.size())));

    JsonValue root = JsonValue::make_object();
    root.set(document.arena().intern("jsonrpc"),
             JsonValue::make_string(document.arena().intern("2.0")));
    if (id.has_value()) root.set(document.arena().intern("id"), id_value(document, *id));
    root.set(document.arena().intern("error"), std::move(error));
    document.root() = std::move(root);
    return from_document(std::move(document));
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

std::optional<JsonRpcId> JsonRpcMessage::copy_id() const
{
    const auto* value = id();
    if (value == nullptr) return std::nullopt;
    if (value->is_string())
        return JsonRpcId(JsonRpcId::Storage(value->as_string().to_std_string()));
    if (value->is_int()) return JsonRpcId(JsonRpcId::Storage(value->as_int()));
    return JsonRpcId(JsonRpcId::Storage(value->as_float()));
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
