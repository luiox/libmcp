#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "mcp/tool_registry.hpp"

namespace mcp::test {
namespace {

const ca::json::JsonValue* member(const ca::json::JsonValue& object, const char* key)
{
    return object.find(ca::str::Utf8StringRef::from_cstr(key));
}

JsonRpcMessage parse_message(const char* input)
{
    auto parsed = JsonRpcMessage::parse(ca::str::Utf8StringRef::from_cstr(input));
    EXPECT_TRUE(parsed.is_ok());
    return std::move(parsed).unwrap();
}

ToolDefinition parse_tool(const char* input)
{
    auto parsed = ToolDefinition::parse(ca::str::Utf8StringRef::from_cstr(input));
    EXPECT_TRUE(parsed.is_ok());
    return std::move(parsed).unwrap();
}

JsonRpcMessage take_response(McpResult<std::optional<JsonRpcMessage>> result)
{
    EXPECT_TRUE(result.is_ok());
    auto response = std::move(result).unwrap();
    EXPECT_TRUE(response.has_value());
    return std::move(*response);
}

ca::i64 error_code(const JsonRpcMessage& response)
{
    const auto* error = response.error();
    EXPECT_NE(error, nullptr);
    const auto* code = member(*error, "code");
    EXPECT_NE(code, nullptr);
    return code->as_int();
}

ServerSession make_server(const std::shared_ptr<ToolRegistry>& registry)
{
    ServerOptions options;
    options.name    = "tool-test";
    options.version = "1.0";
    auto created    = ServerSession::create(std::move(options));
    EXPECT_TRUE(created.is_ok());
    auto server    = std::move(created).unwrap();
    auto installed = server.install_tools(registry);
    EXPECT_TRUE(installed.is_ok());
    return server;
}

JsonRpcMessage initialize(ServerSession& server)
{
    auto request = parse_message(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"client","version":"1"}}})");
    auto response = take_response(server.handle(request));
    auto ready    = parse_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    auto handled  = server.handle(ready);
    EXPECT_TRUE(handled.is_ok());
    return response;
}

MethodResult text_result(const ca::json::JsonValue& arguments)
{
    const auto* text = member(arguments, "text");
    if (text == nullptr || !text->is_string())
        return ca::core::Err(MethodError::invalid_params("text is required"));

    ca::json::JsonDocument document;
    auto                   content_item = ca::json::JsonValue::make_object();
    content_item.set(document.arena().intern("type"),
                     ca::json::JsonValue::make_string(document.arena().intern("text")));
    content_item.set(document.arena().intern("text"),
                     ca::json::JsonValue::make_string(document.arena().intern(text->as_string())));
    auto content = ca::json::JsonValue::make_array();
    content.append(std::move(content_item));
    auto result = ca::json::JsonValue::make_object();
    result.set(document.arena().intern("content"), std::move(content));
    result.set(document.arena().intern("isError"), ca::json::JsonValue::make_bool(false));
    document.root() = std::move(result);
    return ca::core::Ok(std::move(document));
}

}   // namespace

TEST(ToolDefinitionTest, PreservesCompleteDescriptorAndRejectsInvalidKnownFields)
{
    auto definition = parse_tool(
        R"({"name":"echo","title":"Echo","description":"Echo text","inputSchema":{"type":"object","properties":{"text":{"type":"string"}}},"outputSchema":{"type":"object"},"icons":[],"annotations":{"readOnlyHint":true},"execution":{"taskSupport":"forbidden"},"x-extension":{"stable":true}})");
    EXPECT_EQ(definition.name(), "echo");
    EXPECT_NE(member(definition.document().root(), "x-extension"), nullptr);

    EXPECT_TRUE(ToolDefinition::parse(
                    ca::str::Utf8StringRef::from_cstr(R"({"name":"bad","inputSchema":[]})"))
                    .is_err());
    EXPECT_TRUE(ToolDefinition::parse(ca::str::Utf8StringRef::from_cstr(
                                          R"({"name":"bad","inputSchema":{},"icons":{}})"))
                    .is_err());
}

TEST(ToolRegistryTest, ListsDescriptorsAndAdvertisesToolsCapability)
{
    auto registry   = std::make_shared<ToolRegistry>();
    auto registered = registry->register_tool(
        parse_tool(
            R"({"name":"echo","description":"Echo text","inputSchema":{"type":"object"},"x-extension":{"stable":true}})"),
        text_result);
    ASSERT_TRUE(registered.is_ok());
    EXPECT_EQ(registry->size(), 1U);

    auto server = make_server(registry);
    EXPECT_TRUE(
        registry
            ->register_tool(parse_tool(R"({"name":"late","inputSchema":{"type":"object"}})"),
                            text_result)
            .is_err());
    auto        initialize_response = initialize(server);
    const auto* capabilities        = member(*initialize_response.result(), "capabilities");
    ASSERT_NE(capabilities, nullptr);
    const auto* tools_capability = member(*capabilities, "tools");
    ASSERT_NE(tools_capability, nullptr);
    EXPECT_FALSE(member(*tools_capability, "listChanged")->as_bool());

    registry.reset();
    auto        list_request  = parse_message(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    auto        list_response = take_response(server.handle(list_request));
    const auto* tools         = member(*list_response.result(), "tools");
    ASSERT_NE(tools, nullptr);
    ASSERT_EQ(tools->as_array().size(), 1U);
    const auto& descriptor = tools->as_array().front();
    EXPECT_EQ(member(descriptor, "name")->as_string(), "echo");
    EXPECT_NE(member(descriptor, "x-extension"), nullptr);
}

TEST(ToolRegistryTest, CallsToolAndMapsInvalidInputs)
{
    auto registry = std::make_shared<ToolRegistry>();
    ASSERT_TRUE(
        registry
            ->register_tool(parse_tool(R"({"name":"echo","inputSchema":{"type":"object"}})"),
                            text_result)
            .is_ok());
    auto server = make_server(registry);
    initialize(server);

    auto call = parse_message(
        R"({"jsonrpc":"2.0","id":"call-1","method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})");
    auto response = take_response(server.handle(call));
    ASSERT_NE(response.result(), nullptr);
    const auto* content = member(*response.result(), "content");
    ASSERT_NE(content, nullptr);
    ASSERT_EQ(content->as_array().size(), 1U);
    EXPECT_EQ(member(content->as_array().front(), "text")->as_string(), "hello");

    auto missing = parse_message(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"missing","arguments":{}}})");
    EXPECT_EQ(error_code(take_response(server.handle(missing))), -32602);

    auto bad_arguments = parse_message(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":[]}})");
    EXPECT_EQ(error_code(take_response(server.handle(bad_arguments))), -32602);

    auto cursor = parse_message(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/list","params":{"cursor":"next"}})");
    EXPECT_EQ(error_code(take_response(server.handle(cursor))), -32602);
}

TEST(ToolRegistryTest, RejectsDuplicatesInvalidResultsAndLateInstallation)
{
    auto registry   = std::make_shared<ToolRegistry>();
    auto definition = parse_tool(R"({"name":"broken","inputSchema":{"type":"object"}})");
    ASSERT_TRUE(registry
                    ->register_tool(std::move(definition),
                                    [](const ca::json::JsonValue&) -> MethodResult {
                                        return ca::core::Ok(ca::json::JsonDocument());
                                    })
                    .is_ok());
    EXPECT_TRUE(
        registry
            ->register_tool(parse_tool(R"({"name":"broken","inputSchema":{"type":"object"}})"),
                            text_result)
            .is_err());

    auto server = make_server(registry);
    initialize(server);
    auto call = parse_message(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"broken","arguments":{}}})");
    EXPECT_EQ(error_code(take_response(server.handle(call))), -32603);
    EXPECT_TRUE(server.install_tools(std::make_shared<ToolRegistry>()).is_err());
}

}   // namespace mcp::test
