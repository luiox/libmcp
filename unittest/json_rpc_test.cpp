#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "mcp/json_rpc.hpp"

namespace mcp::test {
namespace {

McpResult<JsonRpcMessage> parse(std::string_view input)
{
    return JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(input));
}

TEST(JsonRpcMessageTest, ParsesAllMcpMessageShapes)
{
    auto request = parse(R"({"jsonrpc":"2.0","id":"abc","method":"tools/list","params":{}})");
    ASSERT_TRUE(request.is_ok()) << request.unwrap_err().to_string();
    auto request_message = std::move(request).unwrap();
    EXPECT_EQ(request_message.kind(), JsonRpcMessageKind::Request);
    ASSERT_NE(request_message.id(), nullptr);
    EXPECT_EQ(request_message.id()->as_string(), "abc");
    ASSERT_TRUE(request_message.method().has_value());
    EXPECT_EQ(*request_message.method(), "tools/list");
    ASSERT_NE(request_message.params(), nullptr);
    EXPECT_TRUE(request_message.params()->is_object());

    auto numeric_request = parse(R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})");
    ASSERT_TRUE(numeric_request.is_ok());
    auto numeric_request_message = std::move(numeric_request).unwrap();
    ASSERT_NE(numeric_request_message.id(), nullptr);
    EXPECT_TRUE(numeric_request_message.id()->is_float());

    auto notification = parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    ASSERT_TRUE(notification.is_ok());
    auto notification_message = std::move(notification).unwrap();
    EXPECT_EQ(notification_message.kind(), JsonRpcMessageKind::Notification);

    auto result = parse(R"({"jsonrpc":"2.0","id":7,"result":{"tools":[]}})");
    ASSERT_TRUE(result.is_ok());
    auto result_message = std::move(result).unwrap();
    EXPECT_EQ(result_message.kind(), JsonRpcMessageKind::ResultResponse);
    EXPECT_NE(result_message.result(), nullptr);

    auto error = parse(R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"}})");
    ASSERT_TRUE(error.is_ok());
    auto error_message = std::move(error).unwrap();
    EXPECT_EQ(error_message.kind(), JsonRpcMessageKind::ErrorResponse);
    EXPECT_EQ(error_message.id(), nullptr);
    EXPECT_NE(error_message.error(), nullptr);
}

TEST(JsonRpcMessageTest, OwnsStringsAndRoundTripsCompactJson)
{
    std::string input =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"inspect"}})";
    auto parsed = parse(input);
    ASSERT_TRUE(parsed.is_ok());
    auto message = std::move(parsed).unwrap();
    input.assign(input.size(), 'x');

    ASSERT_TRUE(message.method().has_value());
    EXPECT_EQ(*message.method(), "tools/call");
    auto encoded = message.serialize();
    EXPECT_EQ(encoded.to_std_string(),
              R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"inspect"}})");
}

TEST(JsonRpcMessageTest, RejectsInvalidJsonUtf8AndBatch)
{
    EXPECT_EQ(parse("{").unwrap_err().kind(), McpErrorKind::InvalidJson);
    EXPECT_EQ(parse("[]").unwrap_err().kind(), McpErrorKind::InvalidMessage);

    const std::string invalid_utf8 =
        std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") + static_cast<char>(0xff) + "\"}";
    EXPECT_EQ(parse(invalid_utf8).unwrap_err().kind(), McpErrorKind::InvalidJson);
}

TEST(JsonRpcMessageTest, RejectsAmbiguousOrNonMcpShapes)
{
    const char* invalid_messages[] = {
        R"({"jsonrpc":"1.0","id":1,"method":"ping"})",
        R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
        R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[]})",
        R"({"jsonrpc":"2.0","id":1,"result":[]})",
        R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":-1,"message":"bad"}})",
        R"({"jsonrpc":"2.0","id":1})",
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-1.5,"message":"bad"}})",
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-1}})",
        R"({"jsonrpc":"2.0","method":"ping","result":{}})"};

    for (const char* input : invalid_messages) {
        auto parsed = parse(input);
        EXPECT_TRUE(parsed.is_err()) << input;
        if (parsed.is_err())
            EXPECT_EQ(parsed.unwrap_err().kind(), McpErrorKind::InvalidMessage) << input;
    }
}

}   // namespace
}   // namespace mcp::test
