#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "mcp/server.hpp"

namespace mcp::test {
namespace {

class MemoryReader final : public ca::io::Reader
{
public:
    explicit MemoryReader(std::string data)
        : data_(std::move(data))
    {}

    ca::io::IoResult<ca::usize> read(ca::u8* buffer, ca::usize capacity) override
    {
        if (position_ == data_.size()) return ca::core::Ok(static_cast<ca::usize>(0));
        const ca::usize count = std::min(capacity, data_.size() - position_);
        std::memcpy(buffer, data_.data() + position_, count);
        position_ += count;
        return ca::core::Ok(count);
    }

private:
    std::string data_;
    ca::usize   position_{0};
};

class MemoryWriter final : public ca::io::Writer
{
public:
    ca::io::IoResult<ca::usize> write(const ca::u8* data, ca::usize length) override
    {
        output_.append(reinterpret_cast<const char*>(data), length);
        return ca::core::Ok(length);
    }

    ca::io::IoResult<void> flush() override { return ca::core::Ok(); }

    const std::string& output() const noexcept { return output_; }

private:
    std::string output_;
};

JsonRpcMessage parse_message(const char* input)
{
    auto parsed = JsonRpcMessage::parse(ca::str::Utf8StringRef::from_cstr(input));
    EXPECT_TRUE(parsed.is_ok());
    return std::move(parsed).unwrap();
}

ServerSession make_server()
{
    ServerOptions options;
    options.name                                = "libmcp-test";
    options.version                             = "1.0.0";
    options.title                               = "libmcp test server";
    options.supported_protocol_versions         = {"2025-11-25", "2025-03-26"};
    options.capabilities.tools                  = true;
    options.capabilities.tools_list_changed     = true;
    options.capabilities.resources              = true;
    options.capabilities.resources_list_changed = false;
    auto server                                 = ServerSession::create(std::move(options));
    EXPECT_TRUE(server.is_ok());
    return std::move(server).unwrap();
}

JsonRpcMessage take_response(McpResult<std::optional<JsonRpcMessage>> result)
{
    EXPECT_TRUE(result.is_ok());
    auto response = std::move(result).unwrap();
    EXPECT_TRUE(response.has_value());
    return std::move(*response);
}

const ca::json::JsonValue* member(const ca::json::JsonValue& object, const char* key)
{
    return object.find(ca::str::Utf8StringRef::from_cstr(key));
}

ca::i64 error_code(const JsonRpcMessage& response)
{
    const auto* error = response.error();
    EXPECT_NE(error, nullptr);
    const auto* code = member(*error, "code");
    EXPECT_NE(code, nullptr);
    return code->as_int();
}

void initialize(ServerSession& server)
{
    auto request = parse_message(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test-client","version":"1"}}})");
    auto response = take_response(server.handle(request));
    ASSERT_NE(response.result(), nullptr);
    auto initialized = parse_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    auto handled     = server.handle(initialized);
    ASSERT_TRUE(handled.is_ok());
    EXPECT_FALSE(std::move(handled).unwrap().has_value());
    EXPECT_EQ(server.state(), ServerSessionState::Ready);
}

}   // namespace

TEST(ServerSessionTest, NegotiatesVersionAndTransitionsAfterInitializedNotification)
{
    auto server             = make_server();
    auto initialize_request = parse_message(
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"unsupported","capabilities":{},"clientInfo":{"name":"client","version":"2"}}})");
    auto initialize_response = take_response(server.handle(initialize_request));

    EXPECT_EQ(server.state(), ServerSessionState::AwaitingInitialized);
    EXPECT_EQ(server.negotiated_protocol_version(), "2025-11-25");
    ASSERT_NE(initialize_response.result(), nullptr);
    const auto* protocol_version = member(*initialize_response.result(), "protocolVersion");
    ASSERT_NE(protocol_version, nullptr);
    EXPECT_EQ(protocol_version->as_string(), "2025-11-25");
    const auto* capabilities = member(*initialize_response.result(), "capabilities");
    ASSERT_NE(capabilities, nullptr);
    ASSERT_NE(member(*capabilities, "tools"), nullptr);
    const auto* server_info = member(*initialize_response.result(), "serverInfo");
    ASSERT_NE(server_info, nullptr);
    EXPECT_EQ(member(*server_info, "name")->as_string(), "libmcp-test");

    auto blocked = parse_message(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    EXPECT_EQ(error_code(take_response(server.handle(blocked))), -32600);

    auto initialized = parse_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    auto handled     = server.handle(initialized);
    ASSERT_TRUE(handled.is_ok());
    EXPECT_FALSE(std::move(handled).unwrap().has_value());
    EXPECT_EQ(server.state(), ServerSessionState::Ready);
}

TEST(ServerSessionTest, ValidatesInitializeAndHandlesPingBeforeInitialization)
{
    auto server = make_server();
    auto ping   = parse_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    auto pong   = take_response(server.handle(ping));
    ASSERT_NE(pong.result(), nullptr);
    EXPECT_TRUE(pong.result()->is_object());
    EXPECT_EQ(server.state(), ServerSessionState::AwaitingInitialize);

    auto invalid = parse_message(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    EXPECT_EQ(error_code(take_response(server.handle(invalid))), -32602);
    EXPECT_EQ(server.state(), ServerSessionState::AwaitingInitialize);
}

TEST(ServerSessionTest, DispatchesRegisteredMethodsAndMapsProtocolErrors)
{
    auto server = make_server();
    auto registered =
        server.register_method("echo", [](const JsonRpcMessage& request) -> MethodResult {
            const auto* params = request.params();
            const auto* text   = params == nullptr ? nullptr : member(*params, "text");
            if (text == nullptr || !text->is_string())
                return ca::core::Err(MethodError::invalid_params("text is required"));
            ca::json::JsonDocument document;
            auto                   result = ca::json::JsonValue::make_object();
            result.set(
                document.arena().intern("text"),
                ca::json::JsonValue::make_string(document.arena().intern(text->as_string())));
            document.root() = std::move(result);
            return ca::core::Ok(std::move(document));
        });
    ASSERT_TRUE(registered.is_ok());
    ASSERT_TRUE(server
                    .register_method("broken",
                                     [](const JsonRpcMessage&) -> MethodResult {
                                         return ca::core::Ok(ca::json::JsonDocument());
                                     })
                    .is_ok());
    EXPECT_TRUE(server
                    .register_method("echo",
                                     [](const JsonRpcMessage&) -> MethodResult {
                                         return ca::core::Err(MethodError::internal("unused"));
                                     })
                    .is_err());
    initialize(server);

    auto echo =
        parse_message(R"({"jsonrpc":"2.0","id":"e1","method":"echo","params":{"text":"hello"}})");
    auto echoed = take_response(server.handle(echo));
    ASSERT_NE(echoed.result(), nullptr);
    EXPECT_EQ(member(*echoed.result(), "text")->as_string(), "hello");
    ASSERT_TRUE(echoed.copy_id().has_value());
    EXPECT_EQ(*echoed.copy_id()->string_value(), "e1");

    auto invalid = parse_message(R"({"jsonrpc":"2.0","id":3,"method":"echo"})");
    EXPECT_EQ(error_code(take_response(server.handle(invalid))), -32602);

    auto missing = parse_message(R"({"jsonrpc":"2.0","id":4,"method":"missing"})");
    EXPECT_EQ(error_code(take_response(server.handle(missing))), -32601);

    auto broken = parse_message(R"({"jsonrpc":"2.0","id":5,"method":"broken"})");
    EXPECT_EQ(error_code(take_response(server.handle(broken))), -32603);
}

TEST(ServerSessionTest, ServesRecoverableParseErrorsAndRequestsOverStdio)
{
    MemoryReader reader("not-json\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n");
    MemoryWriter writer;
    auto         transport_result = StdioTransport::create(reader, writer);
    ASSERT_TRUE(transport_result.is_ok());
    auto transport = std::move(transport_result).unwrap();
    auto server    = make_server();

    auto served = server.serve_stdio(transport);
    ASSERT_TRUE(served.is_ok()) << served.unwrap_err().to_string();

    const auto first_end = writer.output().find('\n');
    ASSERT_NE(first_end, std::string::npos);
    auto first = JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(
        std::string_view(writer.output().data(), first_end)));
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(error_code(std::move(first).unwrap()), -32700);

    const auto second_start = first_end + 1;
    const auto second_end   = writer.output().find('\n', second_start);
    ASSERT_NE(second_end, std::string::npos);
    auto second = JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(
        std::string_view(writer.output().data() + second_start, second_end - second_start)));
    ASSERT_TRUE(second.is_ok());
    auto pong = std::move(second).unwrap();
    ASSERT_NE(pong.result(), nullptr);
    EXPECT_TRUE(pong.result()->is_object());
}

TEST(ServerSessionTest, RejectsInvalidConfigurationAndBuiltInRegistration)
{
    ServerOptions invalid;
    EXPECT_TRUE(ServerSession::create(std::move(invalid)).is_err());

    auto server = make_server();
    EXPECT_TRUE(server
                    .register_method("ping",
                                     [](const JsonRpcMessage&) -> MethodResult {
                                         ca::json::JsonDocument result;
                                         result.root() = ca::json::JsonValue::make_object();
                                         return ca::core::Ok(std::move(result));
                                     })
                    .is_err());
    EXPECT_TRUE(server.register_method("unused", MethodHandler()).is_err());
}

}   // namespace mcp::test
