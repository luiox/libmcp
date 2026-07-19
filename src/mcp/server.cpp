#include "mcp/server.hpp"

#include <utility>

#include <libca/str/utf8_util.hpp>

namespace mcp {
namespace {

using ca::json::JsonDocument;
using ca::json::JsonValue;

constexpr ca::i64 JSON_RPC_INVALID_REQUEST  = -32600;
constexpr ca::i64 JSON_RPC_METHOD_NOT_FOUND = -32601;
constexpr ca::i64 JSON_RPC_INVALID_PARAMS   = -32602;
constexpr ca::i64 JSON_RPC_INTERNAL_ERROR   = -32603;
constexpr ca::i64 JSON_RPC_PARSE_ERROR      = -32700;

bool valid_utf8(const std::string& value) noexcept
{
    return ca::str::utf8_is_valid(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

const JsonValue* member(const JsonValue& object, const char* name) noexcept
{
    if (!object.is_object()) return nullptr;
    return object.find(ca::str::Utf8StringRef::from_cstr(name));
}

void set_string(JsonDocument& document, JsonValue& object, const char* key,
                const std::string& value)
{
    object.set(document.arena().intern(key),
               JsonValue::make_string(document.arena().intern(
                   reinterpret_cast<const ca::u8*>(value.data()), value.size())));
}

void set_bool(JsonDocument& document, JsonValue& object, const char* key, bool value)
{
    object.set(document.arena().intern(key), JsonValue::make_bool(value));
}

McpResult<void> validate_options(const ServerOptions& options)
{
    if (options.name.empty() || options.version.empty())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP server name and version must not be empty"));
    const std::string* text_fields[] = {&options.name,
                                        &options.version,
                                        &options.title,
                                        &options.description,
                                        &options.instructions};
    for (const auto* value : text_fields) {
        if (!valid_utf8(*value))
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                     "MCP server option is not valid UTF-8"));
    }
    if (options.supported_protocol_versions.empty())
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "MCP server must support at least one protocol version"));
    for (ca::usize index = 0; index < options.supported_protocol_versions.size(); ++index) {
        const auto& version = options.supported_protocol_versions[index];
        if (version.empty() || !valid_utf8(version))
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidState,
                "MCP supported protocol versions must be non-empty UTF-8 strings"));
        for (ca::usize previous = 0; previous < index; ++previous) {
            if (version == options.supported_protocol_versions[previous])
                return ca::core::Err(McpError::from_kind(
                    McpErrorKind::InvalidState,
                    "MCP supported protocol versions must not contain duplicates"));
        }
    }
    return ca::core::Ok();
}

bool valid_initialize_params(const JsonRpcMessage& message)
{
    const auto* params = message.params();
    if (params == nullptr || !params->is_object()) return false;
    const auto* protocol_version = member(*params, "protocolVersion");
    const auto* capabilities     = member(*params, "capabilities");
    const auto* client_info      = member(*params, "clientInfo");
    if (protocol_version == nullptr || !protocol_version->is_string() || capabilities == nullptr ||
        !capabilities->is_object() || client_info == nullptr || !client_info->is_object())
        return false;
    const auto* name    = member(*client_info, "name");
    const auto* version = member(*client_info, "version");
    return name != nullptr && name->is_string() && version != nullptr && version->is_string();
}

std::optional<JsonRpcMessage> take_message(McpResult<JsonRpcMessage> result)
{
    return std::optional<JsonRpcMessage>(std::move(result).unwrap());
}

}   // namespace

MethodError::MethodError(ca::i64 code, std::string message) noexcept
    : code_(code)
    , message_(std::move(message))
{}

MethodError MethodError::from_code(ca::i64 code, std::string message)
{
    return MethodError(code, std::move(message));
}

MethodError MethodError::invalid_params(std::string message)
{
    return MethodError(JSON_RPC_INVALID_PARAMS, std::move(message));
}

MethodError MethodError::internal(std::string message)
{
    return MethodError(JSON_RPC_INTERNAL_ERROR, std::move(message));
}

ca::i64 MethodError::code() const noexcept
{
    return code_;
}

const std::string& MethodError::message() const noexcept
{
    return message_;
}

ServerSession::ServerSession(ServerOptions options) noexcept
    : options_(std::move(options))
{}

McpResult<ServerSession> ServerSession::create(ServerOptions options)
{
    auto valid = validate_options(options);
    if (valid.is_err()) return ca::core::Err(std::move(valid).unwrap_err());
    return ca::core::Ok(ServerSession(std::move(options)));
}

McpResult<void> ServerSession::register_method(std::string method, MethodHandler handler)
{
    if (method.empty() || !valid_utf8(method))
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "registered MCP method must be a non-empty UTF-8 string"));
    if (!handler)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "registered MCP method handler must not be empty"));
    if (method == "initialize" || method == "ping" || method == "notifications/initialized")
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "cannot replace a built-in MCP lifecycle method"));
    if (handlers_.find(method) != handlers_.end())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP method is already registered: " + method));
    handlers_.emplace(std::move(method), std::move(handler));
    return ca::core::Ok();
}

McpResult<std::optional<JsonRpcMessage>> ServerSession::handle(const JsonRpcMessage& message)
{
    if (message.kind() == JsonRpcMessageKind::ResultResponse ||
        message.kind() == JsonRpcMessageKind::ErrorResponse)
        return ca::core::Ok(std::optional<JsonRpcMessage>{});

    const auto method_ref = message.method();
    if (!method_ref.has_value())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "incoming request is missing a method"));
    const std::string method = method_ref->to_std_string();

    if (method == "initialize") return handle_initialize(message);
    if (method == "notifications/initialized") {
        if (message.kind() == JsonRpcMessageKind::Notification &&
            state_ == ServerSessionState::AwaitingInitialized)
            state_ = ServerSessionState::Ready;
        if (message.kind() == JsonRpcMessageKind::Request)
            return make_error_response(message,
                                       JSON_RPC_INVALID_REQUEST,
                                       "notifications/initialized must be a notification");
        return ca::core::Ok(std::optional<JsonRpcMessage>{});
    }
    if (method == "ping") {
        if (message.kind() == JsonRpcMessageKind::Notification)
            return ca::core::Ok(std::optional<JsonRpcMessage>{});
        return make_empty_result(message);
    }
    if (message.kind() == JsonRpcMessageKind::Notification)
        return ca::core::Ok(std::optional<JsonRpcMessage>{});
    return handle_request(message, method);
}

McpResult<void> ServerSession::serve_stdio(StdioTransport& transport)
{
    for (;;) {
        auto incoming = transport.read_message();
        if (incoming.is_err()) {
            auto    error = std::move(incoming).unwrap_err();
            ca::i64 code  = 0;
            if (error.kind() == McpErrorKind::InvalidJson)
                code = JSON_RPC_PARSE_ERROR;
            else if (error.kind() == McpErrorKind::InvalidMessage)
                code = JSON_RPC_INVALID_REQUEST;
            else
                return ca::core::Err(std::move(error));

            auto response = JsonRpcMessage::make_error(std::nullopt, code, error.message());
            if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
            auto written = transport.write_message(std::move(response).unwrap());
            if (written.is_err()) return ca::core::Err(std::move(written).unwrap_err());
            continue;
        }

        auto message = std::move(incoming).unwrap();
        if (!message.has_value()) return ca::core::Ok();
        auto response = handle(*message);
        if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
        auto outbound = std::move(response).unwrap();
        if (!outbound.has_value()) continue;
        auto written = transport.write_message(*outbound);
        if (written.is_err()) return ca::core::Err(std::move(written).unwrap_err());
    }
}

ServerSessionState ServerSession::state() const noexcept
{
    return state_;
}

const std::string& ServerSession::negotiated_protocol_version() const noexcept
{
    return negotiated_protocol_version_;
}

McpResult<std::optional<JsonRpcMessage>> ServerSession::handle_initialize(
    const JsonRpcMessage& message)
{
    if (message.kind() != JsonRpcMessageKind::Request)
        return ca::core::Ok(std::optional<JsonRpcMessage>{});
    if (state_ != ServerSessionState::AwaitingInitialize)
        return make_error_response(
            message, JSON_RPC_INVALID_REQUEST, "initialize request has already been handled");
    if (!valid_initialize_params(message))
        return make_error_response(
            message,
            JSON_RPC_INVALID_PARAMS,
            "initialize params require protocolVersion, capabilities and clientInfo");

    const auto* requested        = member(*message.params(), "protocolVersion");
    negotiated_protocol_version_ = options_.supported_protocol_versions.front();
    for (const auto& supported : options_.supported_protocol_versions) {
        if (requested->as_string() == supported.c_str()) {
            negotiated_protocol_version_ = supported;
            break;
        }
    }

    auto response = JsonRpcMessage::make_result(
        *message.copy_id(), make_initialize_result(negotiated_protocol_version_));
    if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
    state_ = ServerSessionState::AwaitingInitialized;
    return ca::core::Ok(take_message(std::move(response)));
}

McpResult<std::optional<JsonRpcMessage>> ServerSession::handle_request(
    const JsonRpcMessage& message, const std::string& method)
{
    if (state_ != ServerSessionState::Ready)
        return make_error_response(
            message, JSON_RPC_INVALID_REQUEST, "MCP server has not completed initialization");
    const auto found = handlers_.find(method);
    if (found == handlers_.end())
        return make_error_response(
            message, JSON_RPC_METHOD_NOT_FOUND, "MCP method is not registered: " + method);

    auto result = found->second(message);
    if (result.is_err()) {
        auto error = std::move(result).unwrap_err();
        return make_error_response(message, error.code(), error.message());
    }
    auto response = JsonRpcMessage::make_result(*message.copy_id(), std::move(result).unwrap());
    if (response.is_err())
        return make_error_response(
            message, JSON_RPC_INTERNAL_ERROR, "MCP method handler returned a non-object result");
    return ca::core::Ok(take_message(std::move(response)));
}

McpResult<std::optional<JsonRpcMessage>> ServerSession::make_error_response(
    const JsonRpcMessage& request, ca::i64 code, std::string message)
{
    auto response = JsonRpcMessage::make_error(request.copy_id(), code, std::move(message));
    if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
    return ca::core::Ok(take_message(std::move(response)));
}

McpResult<std::optional<JsonRpcMessage>> ServerSession::make_empty_result(
    const JsonRpcMessage& request)
{
    JsonDocument result;
    result.root() = JsonValue::make_object();
    auto response = JsonRpcMessage::make_result(*request.copy_id(), std::move(result));
    if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
    return ca::core::Ok(take_message(std::move(response)));
}

JsonDocument ServerSession::make_initialize_result(const std::string& version)
{
    JsonDocument document;
    JsonValue    result       = JsonValue::make_object();
    JsonValue    capabilities = JsonValue::make_object();

    if (options_.capabilities.logging)
        capabilities.set(document.arena().intern("logging"), JsonValue::make_object());
    if (options_.capabilities.completions)
        capabilities.set(document.arena().intern("completions"), JsonValue::make_object());
    if (options_.capabilities.prompts) {
        JsonValue prompts = JsonValue::make_object();
        set_bool(document, prompts, "listChanged", options_.capabilities.prompts_list_changed);
        capabilities.set(document.arena().intern("prompts"), std::move(prompts));
    }
    if (options_.capabilities.resources) {
        JsonValue resources = JsonValue::make_object();
        set_bool(document, resources, "subscribe", options_.capabilities.resources_subscribe);
        set_bool(document, resources, "listChanged", options_.capabilities.resources_list_changed);
        capabilities.set(document.arena().intern("resources"), std::move(resources));
    }
    if (options_.capabilities.tools) {
        JsonValue tools = JsonValue::make_object();
        set_bool(document, tools, "listChanged", options_.capabilities.tools_list_changed);
        capabilities.set(document.arena().intern("tools"), std::move(tools));
    }

    JsonValue server_info = JsonValue::make_object();
    set_string(document, server_info, "name", options_.name);
    set_string(document, server_info, "version", options_.version);
    if (!options_.title.empty()) set_string(document, server_info, "title", options_.title);
    if (!options_.description.empty())
        set_string(document, server_info, "description", options_.description);

    set_string(document, result, "protocolVersion", version);
    result.set(document.arena().intern("capabilities"), std::move(capabilities));
    result.set(document.arena().intern("serverInfo"), std::move(server_info));
    if (!options_.instructions.empty())
        set_string(document, result, "instructions", options_.instructions);
    document.root() = std::move(result);
    return document;
}

}   // namespace mcp
