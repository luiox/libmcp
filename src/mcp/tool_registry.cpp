#include "mcp/tool_registry.hpp"

#include <string>
#include <utility>

#include <libca/str/utf8_util.hpp>

namespace mcp {
namespace {

using ca::json::JsonDocument;
using ca::json::JsonType;
using ca::json::JsonValue;

const JsonValue* member(const JsonValue& object, const char* name) noexcept
{
    if (!object.is_object()) return nullptr;
    return object.find(ca::str::Utf8StringRef::from_cstr(name));
}

McpResult<void> validate_definition(const JsonValue& root)
{
    if (!root.is_object())
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidMessage,
                                                 "MCP Tool descriptor must be an object"));
    const auto* name         = member(root, "name");
    const auto* input_schema = member(root, "inputSchema");
    if (name == nullptr || !name->is_string() || name->as_string().is_empty())
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidMessage, "MCP Tool descriptor requires a non-empty string name"));
    if (input_schema == nullptr || !input_schema->is_object())
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidMessage, "MCP Tool descriptor requires an object inputSchema"));

    const auto* title         = member(root, "title");
    const auto* description   = member(root, "description");
    const auto* output_schema = member(root, "outputSchema");
    const auto* icons         = member(root, "icons");
    const auto* annotations   = member(root, "annotations");
    const auto* execution     = member(root, "execution");
    if ((title != nullptr && !title->is_string()) ||
        (description != nullptr && !description->is_string()) ||
        (output_schema != nullptr && !output_schema->is_object()) ||
        (icons != nullptr && !icons->is_array()) ||
        (annotations != nullptr && !annotations->is_object()) ||
        (execution != nullptr && !execution->is_object()))
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidMessage,
                                "MCP Tool descriptor contains a known field of wrong type"));
    return ca::core::Ok();
}

JsonValue clone_value(JsonDocument& destination, const JsonValue& source)
{
    switch (source.type()) {
    case JsonType::Null: return JsonValue::make_null();
    case JsonType::Bool: return JsonValue::make_bool(source.as_bool());
    case JsonType::Int: return JsonValue::make_int(source.as_int());
    case JsonType::Float: return JsonValue::make_float(source.as_float());
    case JsonType::String:
        return JsonValue::make_string(destination.arena().intern(source.as_string()));
    case JsonType::Array:
    {
        JsonValue result = JsonValue::make_array();
        for (const auto& item : source.as_array()) result.append(clone_value(destination, item));
        return result;
    }
    case JsonType::Object:
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

bool valid_call_result(const JsonValue& root) noexcept
{
    if (!root.is_object()) return false;
    const auto* content            = member(root, "content");
    const auto* structured_content = member(root, "structuredContent");
    const auto* is_error           = member(root, "isError");
    return content != nullptr && content->is_array() &&
           (structured_content == nullptr || structured_content->is_object()) &&
           (is_error == nullptr || is_error->is_bool());
}

std::string parse_error_message(const ca::json::ParseError& error)
{
    return "Tool descriptor JSON parse failed at line " + std::to_string(error.location.line) +
           ", column " + std::to_string(error.location.column) + ": " +
           error.message.to_std_string();
}

}   // namespace

ToolDefinition::ToolDefinition(JsonDocument document) noexcept
    : document_(std::move(document))
{}

McpResult<ToolDefinition> ToolDefinition::parse(const ca::str::Utf8StringRef& input)
{
    if (!ca::str::utf8_is_valid(input.data(), input.byte_length()))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidJson,
                                                 "MCP Tool descriptor is not valid UTF-8"));
    auto parsed = ca::json::JsonReader::read(input);
    if (parsed.is_err()) {
        auto error = std::move(parsed).unwrap_err();
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidJson, parse_error_message(error)));
    }
    return from_document(std::move(parsed).unwrap());
}

McpResult<ToolDefinition> ToolDefinition::from_document(JsonDocument document)
{
    auto valid = validate_definition(document.root());
    if (valid.is_err()) return ca::core::Err(std::move(valid).unwrap_err());
    return ca::core::Ok(ToolDefinition(std::move(document)));
}

ca::str::Utf8StringRef ToolDefinition::name() const noexcept
{
    return member(document_.root(), "name")->as_string();
}

const JsonDocument& ToolDefinition::document() const noexcept
{
    return document_;
}

McpResult<void> ToolRegistry::register_tool(ToolDefinition definition, ToolHandler handler)
{
    if (frozen_)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "MCP tool registry cannot change after installation"));
    if (!handler)
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState, "MCP tool handler must not be empty"));
    const std::string name = definition.name().to_std_string();
    for (const auto& entry : entries_) {
        if (entry.name == name)
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                     "MCP tool is already registered: " + name));
    }
    entries_.push_back(Entry{name, std::move(definition), std::move(handler)});
    return ca::core::Ok();
}

ca::usize ToolRegistry::size() const noexcept
{
    return entries_.size();
}

void ToolRegistry::freeze() noexcept
{
    frozen_ = true;
}

MethodResult ToolRegistry::handle_list(const JsonRpcMessage& request) const
{
    if (request.params() != nullptr && member(*request.params(), "cursor") != nullptr)
        return ca::core::Err(
            MethodError::invalid_params("tools/list pagination cursor is not supported"));

    JsonDocument result;
    JsonValue    tools = JsonValue::make_array();
    for (const auto& entry : entries_)
        tools.append(clone_value(result, entry.definition.document().root()));
    JsonValue root = JsonValue::make_object();
    root.set(result.arena().intern("tools"), std::move(tools));
    result.root() = std::move(root);
    return ca::core::Ok(std::move(result));
}

MethodResult ToolRegistry::handle_call(const JsonRpcMessage& request) const
{
    const auto* params    = request.params();
    const auto* name      = params == nullptr ? nullptr : member(*params, "name");
    const auto* arguments = params == nullptr ? nullptr : member(*params, "arguments");
    if (name == nullptr || !name->is_string() || (arguments != nullptr && !arguments->is_object()))
        return ca::core::Err(
            MethodError::invalid_params("tools/call requires string name and object arguments"));

    const Entry* found = nullptr;
    for (const auto& entry : entries_) {
        if (name->as_string() == entry.name.c_str()) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr)
        return ca::core::Err(
            MethodError::invalid_params("unknown MCP tool: " + name->as_string().to_std_string()));

    JsonValue empty_arguments = JsonValue::make_object();
    auto      result          = found->handler(arguments == nullptr ? empty_arguments : *arguments);
    if (result.is_err()) return ca::core::Err(std::move(result).unwrap_err());
    auto document = std::move(result).unwrap();
    if (!valid_call_result(document.root()))
        return ca::core::Err(
            MethodError::internal("MCP tool handler returned an invalid CallToolResult object"));
    return ca::core::Ok(std::move(document));
}

}   // namespace mcp
