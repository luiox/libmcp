#include "mcp/server.hpp"

#include "mcp/tool_registry.hpp"

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

JsonValue id_to_value(JsonDocument& document, const JsonRpcId& id)
{
    if (const auto* value = id.string_value())
        return JsonValue::make_string(
            document.arena().intern(reinterpret_cast<const ca::u8*>(value->data()), value->size()));
    if (const auto* value = id.integer_value()) return JsonValue::make_int(*value);
    return JsonValue::make_float(*id.number_value());
}

JsonValue clone_value(JsonDocument& destination, const JsonValue& source)
{
    switch (source.type()) {
    case ca::json::JsonType::Null: return JsonValue::make_null();
    case ca::json::JsonType::Bool: return JsonValue::make_bool(source.as_bool());
    case ca::json::JsonType::Int: return JsonValue::make_int(source.as_int());
    case ca::json::JsonType::Float: return JsonValue::make_float(source.as_float());
    case ca::json::JsonType::String:
        return JsonValue::make_string(destination.arena().intern(source.as_string()));
    case ca::json::JsonType::Array:
    {
        JsonValue result = JsonValue::make_array();
        for (const auto& item : source.as_array()) result.append(clone_value(destination, item));
        return result;
    }
    case ca::json::JsonType::Object:
    {
        JsonValue result = JsonValue::make_object();
        for (const auto& item : source.as_object())
            result.set(destination.arena().intern(item.first),
                       clone_value(destination, item.second));
        return result;
    }
    }
    return JsonValue::make_null();
}

std::string join_versions(const std::vector<std::string>& versions)
{
    std::string joined;
    for (const auto& version : versions) {
        if (!joined.empty()) joined += ", ";
        joined += version;
    }
    return joined;
}

/// @brief 返回 request `params._meta` 内声明的 modern 协议版本；缺失时返回 nullptr。
const JsonValue* find_meta_protocol_version(const JsonRpcMessage& message) noexcept
{
    const auto* params = message.params();
    if (params == nullptr) return nullptr;
    const auto* meta = member(*params, "_meta");
    if (meta == nullptr) return nullptr;
    return member(*meta, kMetaProtocolVersion);
}

bool is_supported_modern_version(const JsonValue& requested,
                                 const std::vector<std::string>& modern_versions) noexcept
{
    if (!requested.is_string()) return false;
    for (const auto& version : modern_versions) {
        if (requested.as_string() == version.c_str()) return true;
    }
    return false;
}

McpResult<void> validate_version_pool(const std::vector<std::string>& versions, const char* pool)
{
    for (ca::usize index = 0; index < versions.size(); ++index) {
        const auto& version = versions[index];
        if (version.empty() || !valid_utf8(version))
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidState, "MCP " + std::string(pool) +
                                                " protocol versions must be non-empty UTF-8 "
                                                "strings"));
        for (ca::usize previous = 0; previous < index; ++previous) {
            if (version == versions[previous])
                return ca::core::Err(McpError::from_kind(
                    McpErrorKind::InvalidState, "MCP " + std::string(pool) +
                                                    " protocol versions must not contain "
                                                    "duplicates"));
        }
    }
    return ca::core::Ok();
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
    if (options.modern_protocol_versions.empty())
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState,
            "MCP server must support at least one modern protocol version"));
    auto modern = validate_version_pool(options.modern_protocol_versions, "modern");
    if (modern.is_err()) return ca::core::Err(std::move(modern).unwrap_err());
    auto legacy = validate_version_pool(options.legacy_protocol_versions, "legacy");
    if (legacy.is_err()) return ca::core::Err(std::move(legacy).unwrap_err());
    for (const auto& legacy_version : options.legacy_protocol_versions) {
        for (const auto& modern_version : options.modern_protocol_versions) {
            if (legacy_version == modern_version)
                return ca::core::Err(McpError::from_kind(
                    McpErrorKind::InvalidState,
                    "MCP legacy and modern protocol version pools must not overlap"));
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

JsonValue make_capabilities_value(JsonDocument& document, const ServerCapabilities& capabilities)
{
    JsonValue capabilities_value = JsonValue::make_object();
    if (capabilities.logging)
        capabilities_value.set(document.arena().intern("logging"), JsonValue::make_object());
    if (capabilities.completions)
        capabilities_value.set(document.arena().intern("completions"), JsonValue::make_object());
    if (capabilities.prompts) {
        JsonValue prompts = JsonValue::make_object();
        set_bool(document, prompts, "listChanged", capabilities.prompts_list_changed);
        capabilities_value.set(document.arena().intern("prompts"), std::move(prompts));
    }
    if (capabilities.resources) {
        JsonValue resources = JsonValue::make_object();
        set_bool(document, resources, "subscribe", capabilities.resources_subscribe);
        set_bool(document, resources, "listChanged", capabilities.resources_list_changed);
        capabilities_value.set(document.arena().intern("resources"), std::move(resources));
    }
    if (capabilities.tools) {
        JsonValue tools = JsonValue::make_object();
        set_bool(document, tools, "listChanged", capabilities.tools_list_changed);
        capabilities_value.set(document.arena().intern("tools"), std::move(tools));
    }
    return capabilities_value;
}

/// @brief 按 2026-07-28 无状态语义整备 modern result 信封（resultType/serverInfo/缓存提示）。
/// @details 仅作用于 modern 路径；handler 已提供的字段原样保留，legacy 结果绝不进入本函数。
void envelope_modern_result(const std::string& method, const ServerOptions& options,
                            JsonDocument& document)
{
    JsonValue& result = document.root();
    if (!result.is_object()) return;

    if (member(result, "resultType") == nullptr)
        result.set(document.arena().intern("resultType"),
                   JsonValue::make_string(document.arena().intern(kResultTypeComplete)));

    if (options.attach_server_info) {
        JsonValue* meta = result.find(ca::str::Utf8StringRef::from_cstr("_meta"));
        if (meta == nullptr || !meta->is_object()) {
            JsonValue meta_value = JsonValue::make_object();
            JsonValue server_info = JsonValue::make_object();
            set_string(document, server_info, "name", options.name);
            set_string(document, server_info, "version", options.version);
            meta_value.set(document.arena().intern(kMetaServerInfo), std::move(server_info));
            result.set(document.arena().intern("_meta"), std::move(meta_value));
        }
        else if (member(*meta, kMetaServerInfo) == nullptr) {
            JsonValue server_info = JsonValue::make_object();
            set_string(document, server_info, "name", options.name);
            set_string(document, server_info, "version", options.version);
            meta->set(document.arena().intern(kMetaServerInfo), std::move(server_info));
        }
    }

    if (method == "tools/list" && options.cache_ttl_ms >= 0) {
        if (member(result, "ttlMs") == nullptr)
            result.set(document.arena().intern("ttlMs"), JsonValue::make_int(options.cache_ttl_ms));
        if (member(result, "cacheScope") == nullptr)
            set_string(document, result, "cacheScope", options.cache_scope);
    }
}

/// @brief 构造 server/discover 的 DiscoverResult（规范原文形状，免版本校验）。
JsonDocument make_discover_document(const ServerOptions& options)
{
    JsonDocument document;
    JsonValue result = JsonValue::make_object();
    result.set(document.arena().intern("resultType"),
               JsonValue::make_string(document.arena().intern(kResultTypeComplete)));

    JsonValue versions = JsonValue::make_array();
    for (const auto& version : options.modern_protocol_versions)
        versions.append(JsonValue::make_string(document.arena().intern(
            reinterpret_cast<const ca::u8*>(version.data()), version.size())));
    for (const auto& version : options.legacy_protocol_versions)
        versions.append(JsonValue::make_string(document.arena().intern(
            reinterpret_cast<const ca::u8*>(version.data()), version.size())));
    result.set(document.arena().intern("supportedVersions"), std::move(versions));

    result.set(document.arena().intern("capabilities"),
               make_capabilities_value(document, options.capabilities));

    JsonValue meta        = JsonValue::make_object();
    JsonValue server_info = JsonValue::make_object();
    set_string(document, server_info, "name", options.name);
    set_string(document, server_info, "version", options.version);
    meta.set(document.arena().intern(kMetaServerInfo), std::move(server_info));
    result.set(document.arena().intern("_meta"), std::move(meta));

    if (!options.instructions.empty())
        set_string(document, result, "instructions", options.instructions);
    if (options.cache_ttl_ms >= 0) {
        result.set(document.arena().intern("ttlMs"), JsonValue::make_int(options.cache_ttl_ms));
        set_string(document, result, "cacheScope", options.cache_scope);
    }
    document.root() = std::move(result);
    return document;
}

/// @brief 构造带 data{supported,requested} 诊断的 UnsupportedProtocolVersion 错误响应。
McpResult<std::optional<JsonRpcMessage>> make_unsupported_version_response(
    const JsonRpcMessage& request, const std::vector<std::string>& modern_versions,
    const JsonValue* requested, std::string message)
{
    JsonDocument document;
    JsonValue error = JsonValue::make_object();
    error.set(document.arena().intern("code"),
              JsonValue::make_int(kUnsupportedProtocolVersionError));
    error.set(document.arena().intern("message"),
              JsonValue::make_string(document.arena().intern(
                  reinterpret_cast<const ca::u8*>(message.data()), message.size())));

    JsonValue data      = JsonValue::make_object();
    JsonValue supported = JsonValue::make_array();
    for (const auto& version : modern_versions)
        supported.append(JsonValue::make_string(document.arena().intern(
            reinterpret_cast<const ca::u8*>(version.data()), version.size())));
    data.set(document.arena().intern("supported"), std::move(supported));
    data.set(document.arena().intern("requested"),
             requested == nullptr ? JsonValue::make_null() : clone_value(document, *requested));
    error.set(document.arena().intern("data"), std::move(data));

    JsonValue root = JsonValue::make_object();
    root.set(document.arena().intern("jsonrpc"),
             JsonValue::make_string(document.arena().intern("2.0")));
    if (auto id = request.copy_id(); id.has_value())
        root.set(document.arena().intern("id"), id_to_value(document, *id));
    root.set(document.arena().intern("error"), std::move(error));
    document.root() = std::move(root);

    auto response = JsonRpcMessage::from_document(std::move(document));
    if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
    return ca::core::Ok(take_message(std::move(response)));
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

McpResult<void> ServerSession::install_tools(std::shared_ptr<ToolRegistry> registry)
{
    if (registry == nullptr)
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState, "MCP tool registry must not be null"));
    if (state_ != ServerSessionState::AwaitingInitialize)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "MCP tool registry must be installed before initialize"));
    if (handlers_.find("tools/list") != handlers_.end() ||
        handlers_.find("tools/call") != handlers_.end())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP tools methods are already registered"));

    registry->freeze();
    handlers_.emplace("tools/list", [registry](const JsonRpcMessage& request) {
        return registry->handle_list(request);
    });
    handlers_.emplace("tools/call", [registry](const JsonRpcMessage& request) {
        return registry->handle_call(request);
    });
    options_.capabilities.tools              = true;
    options_.capabilities.tools_list_changed = false;
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

    if (method == "server/discover") {
        // 规范规定的免版本探测入口：不做任何协议版本校验，恒按 modern 语义应答。
        if (message.kind() != JsonRpcMessageKind::Request)
            return ca::core::Ok(std::optional<JsonRpcMessage>{});
        auto response =
            JsonRpcMessage::make_result(*message.copy_id(), make_discover_document(options_));
        if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
        return ca::core::Ok(take_message(std::move(response)));
    }
    if (method == "initialize") {
        if (options_.legacy_protocol_versions.empty())
            return make_unsupported_version_response(
                message,
                options_.modern_protocol_versions,
                nullptr,
                "initialize is not supported because no legacy protocol version is enabled; send "
                "requests carrying _meta " +
                    std::string(kMetaProtocolVersion) + " instead (supported: " +
                    join_versions(options_.modern_protocol_versions) + ")");
        return handle_initialize(message);
    }
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
    if (message.kind() == JsonRpcMessageKind::Notification)
        return ca::core::Ok(std::optional<JsonRpcMessage>{});

    // ── Request 分派（dual-era）：_meta 声明 modern 版本走无状态路径；legacy
    // 关系已建立（initialize 已受理，含 AwaitingInitialized 半握手态）则全走
    // legacy 会话路径——时代选择发生在连接开始，此后不再因缺 _meta 改判。 ──
    if (find_meta_protocol_version(message) != nullptr)
        return handle_modern_request(message, method);
    if (method == "ping") return make_empty_result(message);
    if (state_ != ServerSessionState::AwaitingInitialize)
        return handle_request(message, method);
    return make_unsupported_version_response(
        message,
        options_.modern_protocol_versions,
        nullptr,
        "request is missing _meta " + std::string(kMetaProtocolVersion) +
            " and no legacy session is active; declare a supported modern protocol version "
            "(supported: " +
            join_versions(options_.modern_protocol_versions) + ")");
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
        if (written.is_ok()) continue;

        auto write_error = std::move(written).unwrap_err();
        if (write_error.kind() != McpErrorKind::MessageTooLarge)
            return ca::core::Err(std::move(write_error));

        auto fallback =
            JsonRpcMessage::make_error(message->copy_id(),
                                       JSON_RPC_INTERNAL_ERROR,
                                       "JSON-RPC response exceeds stdio transport message limit");
        if (fallback.is_err()) return ca::core::Err(std::move(fallback).unwrap_err());
        auto fallback_written = transport.write_message(std::move(fallback).unwrap());
        if (fallback_written.is_err())
            return ca::core::Err(std::move(fallback_written).unwrap_err());
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
    negotiated_protocol_version_ = options_.legacy_protocol_versions.front();
    for (const auto& supported : options_.legacy_protocol_versions) {
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

McpResult<std::optional<JsonRpcMessage>> ServerSession::handle_modern_request(
    const JsonRpcMessage& message, const std::string& method)
{
    // 2026-07-28：无状态路径——不检查 lifecycle 状态、不改变任何成员状态。
    const auto* requested = find_meta_protocol_version(message);
    if (requested == nullptr ||
        !is_supported_modern_version(*requested, options_.modern_protocol_versions))
        return make_unsupported_version_response(
            message,
            options_.modern_protocol_versions,
            requested,
            "request _meta " + std::string(kMetaProtocolVersion) + " is not supported");

    if (method == "ping") {
        JsonDocument document;
        document.root() = JsonValue::make_object();
        envelope_modern_result(method, options_, document);
        auto response = JsonRpcMessage::make_result(*message.copy_id(), std::move(document));
        if (response.is_err())
            return make_error_response(
                message, JSON_RPC_INTERNAL_ERROR, "MCP modern ping response could not be built");
        return ca::core::Ok(take_message(std::move(response)));
    }

    const auto found = handlers_.find(method);
    if (found == handlers_.end())
        return make_error_response(
            message, JSON_RPC_METHOD_NOT_FOUND, "MCP method is not registered: " + method);

    auto result = found->second(message);
    if (result.is_err()) {
        auto error = std::move(result).unwrap_err();
        return make_error_response(message, error.code(), error.message());
    }
    auto document = std::move(result).unwrap();
    if (!document.root().is_object())
        return make_error_response(
            message, JSON_RPC_INTERNAL_ERROR, "MCP method handler returned a non-object result");
    envelope_modern_result(method, options_, document);
    auto response = JsonRpcMessage::make_result(*message.copy_id(), std::move(document));
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
    JsonValue result = JsonValue::make_object();

    JsonValue server_info = JsonValue::make_object();
    set_string(document, server_info, "name", options_.name);
    set_string(document, server_info, "version", options_.version);
    if (!options_.title.empty()) set_string(document, server_info, "title", options_.title);
    if (!options_.description.empty())
        set_string(document, server_info, "description", options_.description);

    set_string(document, result, "protocolVersion", version);
    result.set(document.arena().intern("capabilities"),
               make_capabilities_value(document, options_.capabilities));
    result.set(document.arena().intern("serverInfo"), std::move(server_info));
    if (!options_.instructions.empty())
        set_string(document, result, "instructions", options_.instructions);
    document.root() = std::move(result);
    return document;
}

}   // namespace mcp
