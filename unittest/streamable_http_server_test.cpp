#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <libca/http/client.hpp>
#include <libca/http/url.hpp>
#include <libca/net/address.hpp>

#include "mcp/streamable_http_server.hpp"
#include "mcp/tool_registry.hpp"

namespace mcp::test {
namespace {

constexpr std::string_view INITIALIZE =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"http-test","version":"1"}}})";

constexpr std::string_view MODERN_TOOLS_LIST =
    R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})";

constexpr std::string_view MISMATCHED_META_TOOLS_LIST =
    R"({"jsonrpc":"2.0","id":8,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2025-11-25"}}})";

class ServerRunner
{
public:
    explicit ServerRunner(ca::http::HttpServer server)
        : server_(std::move(server))
        , completion_(promise_.get_future())
        , thread_([this] { promise_.set_value(server_.serve()); })
    {}

    ServerRunner(const ServerRunner&)            = delete;
    ServerRunner& operator=(const ServerRunner&) = delete;

    ~ServerRunner()
    {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    ca::http::HttpServer                     server_;
    std::promise<ca::http::HttpResult<void>> promise_;
    std::future<ca::http::HttpResult<void>>  completion_;
    std::thread                              thread_;
};

class TestServer
{
public:
    static std::unique_ptr<TestServer> start(
        HttpSessionFactory          factory,
        StreamableHttpServerOptions adapter_options = StreamableHttpServerOptions())
    {
        auto adapter = StreamableHttpServer::create(std::move(factory), std::move(adapter_options));
        if (adapter.is_err()) {
            ADD_FAILURE() << adapter.unwrap_err().to_string();
            return nullptr;
        }

        ca::http::HttpServerOptions server_options;
        server_options.worker_threads = 8;
        auto bound                    = ca::http::HttpServer::bind(
            ca::net::SocketAddress(ca::net::IpAddress::localhost_v4(), 0), server_options);
        if (bound.is_err()) {
            ADD_FAILURE() << bound.unwrap_err().to_string();
            return nullptr;
        }
        auto server        = std::move(bound).unwrap();
        auto adapter_value = std::move(adapter).unwrap();
        auto installed     = adapter_value.install(server);
        if (installed.is_err()) {
            ADD_FAILURE() << installed.unwrap_err().to_string();
            return nullptr;
        }
        auto address = server.local_address();
        if (address.is_err()) {
            ADD_FAILURE() << address.unwrap_err().to_string();
            return nullptr;
        }
        return std::unique_ptr<TestServer>(
            new TestServer(std::move(adapter_value), address.unwrap(), std::move(server)));
    }

    ca::http::HttpUrl url(std::string_view path = "/mcp") const
    {
        auto parsed = ca::http::HttpUrl::parse(
            "http://127.0.0.1:" + std::to_string(address_.port()) + std::string(path));
        EXPECT_TRUE(parsed.is_ok()) << (parsed.is_err() ? parsed.unwrap_err().to_string() : "");
        return std::move(parsed).unwrap();
    }

    ca::usize session_count() const noexcept { return adapter_.session_count(); }

private:
    TestServer(StreamableHttpServer adapter, ca::net::SocketAddress address,
               ca::http::HttpServer server)
        : adapter_(std::move(adapter))
        , address_(std::move(address))
        , runner_(std::move(server))
    {}

    StreamableHttpServer   adapter_;
    ca::net::SocketAddress address_;
    ServerRunner           runner_;
};

McpResult<ServerSession> make_session()
{
    ServerOptions options;
    options.name    = "libmcp-http-test";
    options.version = "1.0.0";
    return ServerSession::create(std::move(options));
}

HttpSessionFactory make_session_factory()
{
    return [](const HttpSessionContext&) { return make_session(); };
}

/// @brief 创建 modern 无状态路径使用的共享实例：安装一个 echo 工具以支撑 tools/list。
std::shared_ptr<ServerSession> make_shared_server()
{
    auto created = make_session();
    EXPECT_TRUE(created.is_ok()) << (created.is_err() ? created.unwrap_err().to_string() : "");
    if (created.is_err()) return nullptr;
    auto session = std::move(created).unwrap();

    auto registry = std::make_shared<ToolRegistry>();
    auto tool     = ToolDefinition::parse(ca::str::Utf8StringRef::from_cstr(
        R"({"name":"echo","description":"Echo text","inputSchema":{"type":"object"}})"));
    EXPECT_TRUE(tool.is_ok());
    if (tool.is_err()) return nullptr;
    auto registered = registry->register_tool(
        std::move(tool).unwrap(),
        [](const ca::json::JsonValue&) -> MethodResult {
            ca::json::JsonDocument document;
            auto content_item = ca::json::JsonValue::make_object();
            content_item.set(document.arena().intern("type"),
                             ca::json::JsonValue::make_string(document.arena().intern("text")));
            content_item.set(document.arena().intern("text"),
                             ca::json::JsonValue::make_string(document.arena().intern("hello")));
            auto content = ca::json::JsonValue::make_array();
            content.append(std::move(content_item));
            auto result = ca::json::JsonValue::make_object();
            result.set(document.arena().intern("content"), std::move(content));
            result.set(document.arena().intern("isError"), ca::json::JsonValue::make_bool(false));
            document.root() = std::move(result);
            return ca::core::Ok(std::move(document));
        });
    EXPECT_TRUE(registered.is_ok());
    if (registered.is_err()) return nullptr;
    auto installed = session.install_tools(registry);
    EXPECT_TRUE(installed.is_ok());
    if (installed.is_err()) return nullptr;
    return std::make_shared<ServerSession>(std::move(session));
}

ca::core::Bytes body_bytes(std::string_view body)
{
    return ca::core::Bytes::copy_from_slice(reinterpret_cast<const ca::u8*>(body.data()),
                                            body.size());
}

std::string body_text(const ca::core::Bytes& body)
{
    return std::string(reinterpret_cast<const char*>(body.as_ptr()), body.remaining());
}

ca::http::HttpRequest post_request(std::string_view                body,
                                   std::optional<std::string_view> session_id       = std::nullopt,
                                   std::optional<std::string_view> protocol_version = std::nullopt)
{
    ca::http::HttpRequest request;
    request.method = "POST";
    request.body   = body_bytes(body);
    EXPECT_TRUE(request.headers.append("Content-Type", "application/json; charset=utf-8").is_ok());
    EXPECT_TRUE(request.headers.append("Accept", "application/json, text/event-stream").is_ok());
    if (session_id.has_value())
        EXPECT_TRUE(request.headers.append("MCP-Session-Id", std::string(*session_id)).is_ok());
    if (protocol_version.has_value())
        EXPECT_TRUE(
            request.headers.append("MCP-Protocol-Version", std::string(*protocol_version)).is_ok());
    return request;
}

ca::http::HttpRequest with_authorization(ca::http::HttpRequest request,
                                         std::string_view      authorization)
{
    EXPECT_TRUE(request.headers.append("Authorization", std::string(authorization)).is_ok());
    return request;
}

ca::http::HttpResponse send(ca::http::HttpClient& client, const ca::http::HttpUrl& url,
                            ca::http::HttpRequest request)
{
    auto response = client.request(url, std::move(request));
    if (response.is_err()) {
        ADD_FAILURE() << response.unwrap_err().to_string();
        return ca::http::HttpResponse();
    }
    return std::move(response).unwrap();
}

ca::http::HttpClient make_client()
{
    auto created = ca::http::HttpClient::create();
    EXPECT_TRUE(created.is_ok()) << (created.is_err() ? created.unwrap_err().to_string() : "");
    return std::move(created).unwrap();
}

std::string required_session_id(const ca::http::HttpResponse& response)
{
    auto value = response.headers.get("MCP-Session-Id");
    EXPECT_TRUE(value.has_value());
    return value.has_value() ? std::string(*value) : std::string();
}

ca::i64 json_rpc_error_code(const ca::http::HttpResponse& response)
{
    auto parsed =
        JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(body_text(response.body)));
    EXPECT_TRUE(parsed.is_ok()) << (parsed.is_err() ? parsed.unwrap_err().to_string() : "");
    if (parsed.is_err()) return 0;
    auto        message = std::move(parsed).unwrap();
    const auto* error   = message.error();
    EXPECT_NE(error, nullptr);
    if (error == nullptr) return 0;
    const auto* code = error->find(ca::str::Utf8StringRef::from_cstr("code"));
    EXPECT_NE(code, nullptr);
    return code == nullptr ? 0 : code->as_int();
}

struct SseTestEvent
{
    std::string id;
    std::string data;
};

std::vector<SseTestEvent> parse_sse_events(const ca::core::Bytes& body)
{
    const auto                encoded = body_text(body);
    std::vector<SseTestEvent> events;
    ca::usize                 offset = 0;
    while (offset < encoded.size()) {
        const auto ending = encoded.find("\n\n", offset);
        EXPECT_NE(ending, std::string::npos);
        if (ending == std::string::npos) break;

        const auto   block = std::string_view(encoded).substr(offset, ending - offset);
        SseTestEvent event;
        bool         saw_data   = false;
        ca::usize    line_start = 0;
        while (line_start <= block.size()) {
            const auto line_end = block.find('\n', line_start);
            const auto line     = block.substr(line_start, line_end - line_start);
            if (line.substr(0, 4) == "id: ") {
                event.id = std::string(line.substr(4));
            }
            else if (line == "data:" || line.substr(0, 6) == "data: ") {
                if (saw_data) event.data += '\n';
                if (line.size() > 5) event.data.append(line.substr(6));
                saw_data = true;
            }
            if (line_end == std::string_view::npos) break;
            line_start = line_end + 1;
        }
        EXPECT_FALSE(event.id.empty());
        EXPECT_TRUE(saw_data);
        events.push_back(std::move(event));
        offset = ending + 2;
    }
    return events;
}

ca::http::HttpRequest replay_request(std::string_view session_id, std::string_view event_id)
{
    ca::http::HttpRequest request;
    request.method = "GET";
    EXPECT_TRUE(request.headers.append("Accept", "text/event-stream").is_ok());
    EXPECT_TRUE(request.headers.append("MCP-Session-Id", std::string(session_id)).is_ok());
    EXPECT_TRUE(request.headers.append("MCP-Protocol-Version", "2025-11-25").is_ok());
    EXPECT_TRUE(request.headers.append("Last-Event-ID", std::string(event_id)).is_ok());
    return request;
}

TEST(StreamableHttpServerTest, InitializesSessionAndHandlesBufferedMessages)
{
    auto server = TestServer::start(make_session_factory());
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    ASSERT_EQ(initialized.status, 200);
    EXPECT_EQ(initialized.headers.get("Content-Type"), "application/json");
    const auto session_id = required_session_id(initialized);
    EXPECT_EQ(session_id.size(), 64U);
    EXPECT_EQ(server->session_count(), 1U);

    auto initialize_message = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_string_view(body_text(initialized.body)));
    ASSERT_TRUE(initialize_message.is_ok());
    auto initialize_response = std::move(initialize_message).unwrap();
    ASSERT_NE(initialize_response.result(), nullptr);

    auto ready = send(
        client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session_id, "2025-11-25"));
    EXPECT_EQ(ready.status, 202);
    EXPECT_TRUE(ready.body.is_empty());

    auto ping =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-11-25"));
    EXPECT_EQ(ping.status, 200);
    auto ping_message =
        JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(body_text(ping.body)));
    ASSERT_TRUE(ping_message.is_ok());
    auto ping_response = std::move(ping_message).unwrap();
    EXPECT_NE(ping_response.result(), nullptr);

    auto client_response =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":99,"result":{}})", session_id, "2025-11-25"));
    EXPECT_EQ(client_response.status, 202);
}

TEST(StreamableHttpServerTest, StreamsResponsesAndResumesOnlyTheOriginatingStream)
{
    StreamableHttpServerOptions options;
    options.sse.emplace();
    auto server = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    ASSERT_EQ(initialized.status, 200);
    EXPECT_EQ(initialized.headers.get("Content-Type"), "text/event-stream");
    EXPECT_EQ(initialized.headers.get("Cache-Control"), "no-cache");
    const auto session_id        = required_session_id(initialized);
    const auto initialize_events = parse_sse_events(initialized.body);
    ASSERT_EQ(initialize_events.size(), 2U);
    EXPECT_TRUE(initialize_events.front().data.empty());
    EXPECT_NE(initialize_events[0].id, initialize_events[1].id);
    auto initialize_message =
        JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(initialize_events[1].data));
    ASSERT_TRUE(initialize_message.is_ok());
    EXPECT_NE(std::move(initialize_message).unwrap().result(), nullptr);

    auto ready = send(
        client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session_id, "2025-11-25"));
    ASSERT_EQ(ready.status, 202);

    auto first =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-11-25"));
    ASSERT_EQ(first.status, 200);
    const auto first_events = parse_sse_events(first.body);
    ASSERT_EQ(first_events.size(), 2U);
    EXPECT_TRUE(first_events.front().data.empty());

    auto second =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":3,"method":"ping"})", session_id, "2025-11-25"));
    ASSERT_EQ(second.status, 200);
    const auto second_events = parse_sse_events(second.body);
    ASSERT_EQ(second_events.size(), 2U);
    EXPECT_NE(first_events[0].id, second_events[0].id);
    EXPECT_NE(first_events[1].id, second_events[1].id);

    ca::http::HttpRequest listen;
    listen.method = "GET";
    ASSERT_TRUE(listen.headers.append("Accept", "text/event-stream").is_ok());
    ASSERT_TRUE(listen.headers.append("MCP-Session-Id", session_id).is_ok());
    ASSERT_TRUE(listen.headers.append("MCP-Protocol-Version", "2025-11-25").is_ok());
    auto unsupported = send(client, server->url(), std::move(listen));
    EXPECT_EQ(unsupported.status, 405);
    EXPECT_EQ(unsupported.headers.get("Allow"), "POST, DELETE");

    auto missing_accept = replay_request(session_id, first_events.front().id);
    missing_accept.headers.remove("Accept");
    EXPECT_EQ(send(client, server->url(), std::move(missing_accept)).status, 406);

    auto repeated_cursor = replay_request(session_id, first_events.front().id);
    ASSERT_TRUE(repeated_cursor.headers.append("Last-Event-ID", first_events.back().id).is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(repeated_cursor)).status, 400);

    EXPECT_EQ(send(client, server->url(), replay_request(session_id, "unknown:0")).status, 404);

    auto resumed = send(client, server->url(), replay_request(session_id, first_events.front().id));
    ASSERT_EQ(resumed.status, 200);
    EXPECT_EQ(resumed.headers.get("Content-Type"), "text/event-stream");
    const auto resumed_events = parse_sse_events(resumed.body);
    ASSERT_EQ(resumed_events.size(), 1U);
    EXPECT_EQ(resumed_events.front().id, first_events[1].id);
    auto replayed_message = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_string_view(resumed_events.front().data));
    ASSERT_TRUE(replayed_message.is_ok());
    auto replayed_id = std::move(replayed_message).unwrap().copy_id();
    ASSERT_TRUE(replayed_id.has_value());
    ASSERT_NE(replayed_id->integer_value(), nullptr);
    EXPECT_EQ(*replayed_id->integer_value(), 2);

    auto completed =
        send(client, server->url(), replay_request(session_id, first_events.back().id));
    EXPECT_EQ(completed.status, 200);
    EXPECT_TRUE(completed.body.is_empty());
}

TEST(StreamableHttpServerTest, FallsBackToBufferedResponseForHttp10)
{
    StreamableHttpServerOptions options;
    options.sse.emplace();
    auto server = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialize    = post_request(INITIALIZE);
    initialize.version = ca::http::HttpVersion::Http10;
    auto response      = send(client, server->url(), std::move(initialize));
    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(response.headers.get("Content-Type"), "application/json");
    EXPECT_FALSE(required_session_id(response).empty());
    auto message =
        JsonRpcMessage::parse(ca::str::Utf8StringRef::from_string_view(body_text(response.body)));
    ASSERT_TRUE(message.is_ok());
    EXPECT_NE(std::move(message).unwrap().result(), nullptr);
}

TEST(StreamableHttpServerTest, EvictsOldSseReplayStreamsAtConfiguredCapacity)
{
    StreamableHttpServerOptions options;
    options.sse.emplace();
    options.sse->max_replay_streams = 1;
    auto server                     = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    ASSERT_EQ(initialized.status, 200);
    const auto session_id        = required_session_id(initialized);
    const auto initialize_events = parse_sse_events(initialized.body);
    ASSERT_EQ(initialize_events.size(), 2U);

    auto ready = send(
        client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session_id, "2025-11-25"));
    ASSERT_EQ(ready.status, 202);
    auto ping =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-11-25"));
    ASSERT_EQ(ping.status, 200);
    const auto ping_events = parse_sse_events(ping.body);
    ASSERT_EQ(ping_events.size(), 2U);

    EXPECT_EQ(send(client, server->url(), replay_request(session_id, initialize_events.front().id))
                  .status,
              404);
    auto replayed = send(client, server->url(), replay_request(session_id, ping_events.front().id));
    EXPECT_EQ(replayed.status, 200);
    const auto replayed_events = parse_sse_events(replayed.body);
    ASSERT_EQ(replayed_events.size(), 1U);
    EXPECT_EQ(replayed_events.front().id, ping_events.back().id);
}

TEST(StreamableHttpServerTest, SendsButDoesNotRetainSseStreamOverReplayByteLimit)
{
    StreamableHttpServerOptions options;
    options.sse.emplace();
    options.sse->max_replay_bytes = 1;
    auto server                   = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    ASSERT_EQ(initialized.status, 200);
    EXPECT_EQ(initialized.headers.get("Content-Type"), "text/event-stream");
    const auto session_id = required_session_id(initialized);
    const auto events     = parse_sse_events(initialized.body);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_FALSE(events.back().data.empty());
    EXPECT_EQ(send(client, server->url(), replay_request(session_id, events.front().id)).status,
              404);
}

TEST(StreamableHttpServerTest, ValidatesContentNegotiationAndOrigin)
{
    StreamableHttpServerOptions options;
    options.allowed_origins = {"https://allowed.example"};
    auto server             = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto wrong_origin_request = post_request(INITIALIZE);
    ASSERT_TRUE(wrong_origin_request.headers.append("Origin", "https://denied.example").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(wrong_origin_request)).status, 403);

    auto no_content_type = post_request(INITIALIZE);
    no_content_type.headers.remove("Content-Type");
    EXPECT_EQ(send(client, server->url(), std::move(no_content_type)).status, 415);

    auto incomplete_accept = post_request(INITIALIZE);
    ASSERT_TRUE(incomplete_accept.headers.set("Accept", "application/json").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(incomplete_accept)).status, 406);

    auto disabled_sse = post_request(INITIALIZE);
    ASSERT_TRUE(
        disabled_sse.headers.set("Accept", "application/json, text/event-stream;q=0").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(disabled_sse)).status, 406);

    auto malformed_quality = post_request(INITIALIZE);
    ASSERT_TRUE(
        malformed_quality.headers.set("Accept", "application/json;q=0..0, text/event-stream")
            .is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(malformed_quality)).status, 406);

    auto positive_quality = post_request(INITIALIZE);
    ASSERT_TRUE(positive_quality.headers
                    .set("Accept", "application/json;q=0.001, text/event-stream;q=1.000")
                    .is_ok());
    ASSERT_TRUE(positive_quality.headers.append("Origin", "https://allowed.example").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(positive_quality)).status, 200);

    auto allowed = post_request(INITIALIZE);
    ASSERT_TRUE(allowed.headers.append("Origin", "https://allowed.example").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(allowed)).status, 200);
}

TEST(StreamableHttpServerTest, ValidatesSessionAndProtocolHeaders)
{
    auto server = TestServer::start(make_session_factory());
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    EXPECT_EQ(
        send(client, server->url(), post_request(R"({"jsonrpc":"2.0","id":1,"method":"ping"})"))
            .status,
        400);
    EXPECT_EQ(
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "unknown", "2025-11-25"))
            .status,
        404);

    const auto session_id =
        required_session_id(send(client, server->url(), post_request(INITIALIZE)));
    EXPECT_EQ(send(client,
                   server->url(),
                   post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id))
                  .status,
              400);
    EXPECT_EQ(
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-03-26"))
            .status,
        400);
}

TEST(StreamableHttpServerTest, CanUseNegotiatedVersionWhenHeaderRequirementIsDisabled)
{
    StreamableHttpServerOptions options;
    options.require_protocol_version_header = false;
    auto server = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto       client = make_client();
    const auto session_id =
        required_session_id(send(client, server->url(), post_request(INITIALIZE)));

    auto ping = send(client,
                     server->url(),
                     post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id));
    EXPECT_EQ(ping.status, 200);
}

TEST(StreamableHttpServerTest, DeletesSessionAndRejectsDeletedId)
{
    auto server = TestServer::start(make_session_factory());
    ASSERT_NE(server, nullptr);
    auto       client = make_client();
    const auto session_id =
        required_session_id(send(client, server->url(), post_request(INITIALIZE)));

    ca::http::HttpRequest missing;
    missing.method = "DELETE";
    EXPECT_EQ(send(client, server->url(), std::move(missing)).status, 400);

    ca::http::HttpRequest remove;
    remove.method = "DELETE";
    ASSERT_TRUE(remove.headers.append("MCP-Session-Id", session_id).is_ok());
    ASSERT_TRUE(remove.headers.append("MCP-Protocol-Version", "2025-11-25").is_ok());
    auto removed = send(client, server->url(), std::move(remove));
    EXPECT_EQ(removed.status, 204);
    EXPECT_TRUE(removed.body.is_empty());
    EXPECT_EQ(server->session_count(), 0U);

    EXPECT_EQ(
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-11-25"))
            .status,
        404);

    ca::http::HttpRequest remove_again;
    remove_again.method = "DELETE";
    ASSERT_TRUE(remove_again.headers.append("MCP-Session-Id", session_id).is_ok());
    ASSERT_TRUE(remove_again.headers.append("MCP-Protocol-Version", "2025-11-25").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(remove_again)).status, 404);
}

TEST(StreamableHttpServerTest, ChecksOriginBeforeRoutingAndOnlyForMcpEndpoint)
{
    StreamableHttpServerOptions options;
    options.allowed_origins = {"https://allowed.example"};
    auto server             = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    ca::http::HttpRequest get;
    EXPECT_EQ(send(client, server->url(), std::move(get)).status, 405);

    ca::http::HttpRequest denied;
    ASSERT_TRUE(denied.headers.append("Origin", "https://denied.example").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(denied)).status, 403);

    ca::http::HttpRequest denied_method;
    denied_method.method = "PUT";
    ASSERT_TRUE(denied_method.headers.append("Origin", "https://denied.example").is_ok());
    EXPECT_EQ(send(client, server->url("/mcp?q=1"), std::move(denied_method)).status, 403);

    ca::http::HttpRequest allowed_method;
    allowed_method.method = "PUT";
    ASSERT_TRUE(allowed_method.headers.append("Origin", "https://allowed.example").is_ok());
    EXPECT_EQ(send(client, server->url(), std::move(allowed_method)).status, 405);

    ca::http::HttpRequest unrelated;
    ASSERT_TRUE(unrelated.headers.append("Origin", "https://denied.example").is_ok());
    EXPECT_EQ(send(client, server->url("/missing"), std::move(unrelated)).status, 404);
}

TEST(StreamableHttpServerTest, AuthorizesRequestsAndBindsIdentityToSession)
{
    StreamableHttpServerOptions options;
    options.sse.emplace();
    options.authorizer = [](const ca::http::HttpServerRequestContext& context)
        -> ca::http::HttpResult<HttpAuthorizationDecision> {
        const auto values = context.request().headers.get_all("Authorization");
        if (values.size() == 1 && values.front() == "Bearer alice")
            return ca::core::Ok(HttpAuthorizationDecision::authorized("alice"));
        if (values.size() == 1 && values.front() == "Bearer bob")
            return ca::core::Ok(HttpAuthorizationDecision::authorized("bob"));
        if (values.size() == 1 && values.front() == "Bearer empty")
            return ca::core::Ok(HttpAuthorizationDecision::authorized(""));

        ca::http::HttpResponse response;
        response.status = 401;
        auto challenge  = response.headers.append("WWW-Authenticate", "Bearer realm=\"mcp\"");
        if (challenge.is_err()) return ca::core::Err(std::move(challenge).unwrap_err());
        return ca::core::Ok(HttpAuthorizationDecision::rejected(
            ca::http::HttpServerResponse::buffered(std::move(response))));
    };

    std::mutex               identities_mutex;
    std::vector<std::string> factory_identities;
    auto                     server = TestServer::start(
        [&](const HttpSessionContext& context) -> McpResult<ServerSession> {
            std::lock_guard<std::mutex> lock(identities_mutex);
            factory_identities.emplace_back(context.authorization_identity);
            return make_session();
        },
        std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    ca::http::HttpRequest get;
    auto                  unauthorized = send(client, server->url(), std::move(get));
    EXPECT_EQ(unauthorized.status, 401);
    EXPECT_EQ(unauthorized.headers.get("WWW-Authenticate"), "Bearer realm=\"mcp\"");

    auto empty_identity =
        send(client, server->url(), with_authorization(post_request(INITIALIZE), "Bearer empty"));
    EXPECT_EQ(empty_identity.status, 500);
    EXPECT_EQ(server->session_count(), 0U);

    auto initialized =
        send(client, server->url(), with_authorization(post_request(INITIALIZE), "Bearer alice"));
    ASSERT_EQ(initialized.status, 200);
    const auto session_id = required_session_id(initialized);
    {
        std::lock_guard<std::mutex> lock(identities_mutex);
        ASSERT_EQ(factory_identities.size(), 1U);
        EXPECT_EQ(factory_identities.front(), "alice");
    }

    auto ready = send(
        client,
        server->url(),
        with_authorization(post_request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                                        session_id,
                                        "2025-11-25"),
                           "Bearer alice"));
    ASSERT_EQ(ready.status, 202);

    auto wrong_identity = send(
        client,
        server->url(),
        with_authorization(
            post_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session_id, "2025-11-25"),
            "Bearer bob"));
    EXPECT_EQ(wrong_identity.status, 404);
    EXPECT_EQ(server->session_count(), 1U);

    auto ping = send(
        client,
        server->url(),
        with_authorization(
            post_request(R"({"jsonrpc":"2.0","id":3,"method":"ping"})", session_id, "2025-11-25"),
            "Bearer alice"));
    EXPECT_EQ(ping.status, 200);
    const auto ping_events = parse_sse_events(ping.body);
    ASSERT_EQ(ping_events.size(), 2U);

    auto wrong_replay =
        send(client,
             server->url(),
             with_authorization(replay_request(session_id, ping_events.front().id), "Bearer bob"));
    EXPECT_EQ(wrong_replay.status, 404);

    auto allowed_replay = send(
        client,
        server->url(),
        with_authorization(replay_request(session_id, ping_events.front().id), "Bearer alice"));
    EXPECT_EQ(allowed_replay.status, 200);
    const auto allowed_events = parse_sse_events(allowed_replay.body);
    ASSERT_EQ(allowed_events.size(), 1U);
    EXPECT_EQ(allowed_events.front().id, ping_events.back().id);

    ca::http::HttpRequest remove;
    remove.method = "DELETE";
    ASSERT_TRUE(remove.headers.append("MCP-Session-Id", session_id).is_ok());
    ASSERT_TRUE(remove.headers.append("MCP-Protocol-Version", "2025-11-25").is_ok());
    auto removed =
        send(client, server->url(), with_authorization(std::move(remove), "Bearer alice"));
    EXPECT_EQ(removed.status, 204);
    EXPECT_EQ(server->session_count(), 0U);
}

TEST(StreamableHttpServerTest, MapsMalformedAndInvalidMessagesToJsonRpcErrors)
{
    auto server = TestServer::start(make_session_factory());
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto malformed = send(client, server->url(), post_request("{"));
    EXPECT_EQ(malformed.status, 400);
    EXPECT_EQ(json_rpc_error_code(malformed), -32700);

    auto invalid = send(client, server->url(), post_request("[]"));
    EXPECT_EQ(invalid.status, 400);
    EXPECT_EQ(json_rpc_error_code(invalid), -32600);

    auto bad_initialize =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    EXPECT_EQ(bad_initialize.status, 200);
    EXPECT_FALSE(bad_initialize.headers.contains("MCP-Session-Id"));
    EXPECT_EQ(json_rpc_error_code(bad_initialize), -32602);
    EXPECT_EQ(server->session_count(), 0U);
}

TEST(StreamableHttpServerTest, ReportsFactoryFailureAndSessionCapacity)
{
    auto failing = TestServer::start([](const HttpSessionContext&) -> McpResult<ServerSession> {
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState, "factory test failure"));
    });
    ASSERT_NE(failing, nullptr);
    auto failing_client = make_client();
    EXPECT_EQ(send(failing_client, failing->url(), post_request(INITIALIZE)).status, 500);

    StreamableHttpServerOptions throwing_options;
    throwing_options.max_sessions = 1;
    auto throwing                 = TestServer::start(
        [](const HttpSessionContext&) -> McpResult<ServerSession> {
            throw std::runtime_error("factory failure");
        },
        std::move(throwing_options));
    ASSERT_NE(throwing, nullptr);
    auto throwing_client = make_client();
    EXPECT_EQ(send(throwing_client, throwing->url(), post_request(INITIALIZE)).status, 500);
    EXPECT_EQ(send(throwing_client, throwing->url(), post_request(INITIALIZE)).status, 500);
    EXPECT_EQ(throwing->session_count(), 0U);

    StreamableHttpServerOptions options;
    options.max_sessions = 1;
    auto limited         = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(limited, nullptr);
    auto limited_client = make_client();
    EXPECT_EQ(send(limited_client, limited->url(), post_request(INITIALIZE)).status, 200);
    EXPECT_EQ(send(limited_client, limited->url(), post_request(INITIALIZE)).status, 503);
    EXPECT_EQ(limited->session_count(), 1U);
}

TEST(StreamableHttpServerTest, RemovesSessionAfterUnhandledFailure)
{
    auto server = TestServer::start([](const HttpSessionContext&) -> McpResult<ServerSession> {
        auto created = make_session();
        if (created.is_err()) return ca::core::Err(std::move(created).unwrap_err());
        auto session = std::move(created).unwrap();
        auto registered =
            session.register_method("explode", [](const JsonRpcMessage&) -> MethodResult {
                throw std::runtime_error("handler failure");
            });
        if (registered.is_err()) return ca::core::Err(std::move(registered).unwrap_err());
        return ca::core::Ok(std::move(session));
    });
    ASSERT_NE(server, nullptr);
    auto       client = make_client();
    const auto session_id =
        required_session_id(send(client, server->url(), post_request(INITIALIZE)));
    auto ready = send(
        client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session_id, "2025-11-25"));
    ASSERT_EQ(ready.status, 202);

    auto failed = send(
        client,
        server->url(),
        post_request(R"({"jsonrpc":"2.0","id":2,"method":"explode"})", session_id, "2025-11-25"));
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(server->session_count(), 0U);
    auto after_failure =
        send(client,
             server->url(),
             post_request(R"({"jsonrpc":"2.0","id":3,"method":"ping"})", session_id, "2025-11-25"));
    EXPECT_EQ(after_failure.status, 404);
}

TEST(StreamableHttpServerTest, SerializesConcurrentRequestsWithinSession)
{
    struct ConcurrencyState
    {
        std::atomic<ca::i32> active{0};
        std::atomic<ca::i32> maximum{0};
    };
    auto state  = std::make_shared<ConcurrencyState>();
    auto server = TestServer::start([state](const HttpSessionContext&) -> McpResult<ServerSession> {
        auto created = make_session();
        if (created.is_err()) return ca::core::Err(std::move(created).unwrap_err());
        auto session = std::move(created).unwrap();
        auto registered =
            session.register_method("slow", [state](const JsonRpcMessage&) -> MethodResult {
                const auto now  = state->active.fetch_add(1) + 1;
                auto       seen = state->maximum.load();
                while (seen < now && !state->maximum.compare_exchange_weak(seen, now)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                state->active.fetch_sub(1);
                ca::json::JsonDocument result;
                result.root() = ca::json::JsonValue::make_object();
                return ca::core::Ok(std::move(result));
            });
        if (registered.is_err()) return ca::core::Err(std::move(registered).unwrap_err());
        return ca::core::Ok(std::move(session));
    });
    ASSERT_NE(server, nullptr);
    auto       setup_client = make_client();
    const auto session_id =
        required_session_id(send(setup_client, server->url(), post_request(INITIALIZE)));
    auto ready = send(
        setup_client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session_id, "2025-11-25"));
    ASSERT_EQ(ready.status, 202);

    constexpr ca::usize      REQUEST_COUNT = 6;
    std::atomic<bool>        start{false};
    std::vector<std::thread> threads;
    std::vector<ca::u16>     statuses(REQUEST_COUNT, 0);
    for (ca::usize index = 0; index < REQUEST_COUNT; ++index) {
        threads.emplace_back([&, index] {
            auto client = make_client();
            while (!start.load()) std::this_thread::yield();
            auto response =
                send(client,
                     server->url(),
                     post_request("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(index + 10) +
                                      ",\"method\":\"slow\"}",
                                  session_id,
                                  "2025-11-25"));
            statuses[index] = response.status;
        });
    }
    start.store(true);
    for (auto& thread : threads) thread.join();

    for (auto status : statuses) EXPECT_EQ(status, 200);
    EXPECT_EQ(state->maximum.load(), 1);
}

TEST(StreamableHttpServerTest, ValidatesAdapterOptions)
{
    EXPECT_TRUE(StreamableHttpServer::create(HttpSessionFactory()).is_err());

    StreamableHttpServerOptions invalid_endpoint;
    invalid_endpoint.endpoint = "mcp";
    EXPECT_TRUE(
        StreamableHttpServer::create(make_session_factory(), std::move(invalid_endpoint)).is_err());

    StreamableHttpServerOptions invalid_capacity;
    invalid_capacity.max_sessions = 0;
    EXPECT_TRUE(
        StreamableHttpServer::create(make_session_factory(), std::move(invalid_capacity)).is_err());

    StreamableHttpServerOptions invalid_random;
    invalid_random.session_id_bytes = 1;
    EXPECT_TRUE(
        StreamableHttpServer::create(make_session_factory(), std::move(invalid_random)).is_err());

    StreamableHttpServerOptions invalid_replay_streams;
    invalid_replay_streams.sse.emplace();
    invalid_replay_streams.sse->max_replay_streams = 0;
    EXPECT_TRUE(
        StreamableHttpServer::create(make_session_factory(), std::move(invalid_replay_streams))
            .is_err());

    StreamableHttpServerOptions invalid_replay_bytes;
    invalid_replay_bytes.sse.emplace();
    invalid_replay_bytes.sse->max_replay_bytes = 0;
    EXPECT_TRUE(
        StreamableHttpServer::create(make_session_factory(), std::move(invalid_replay_bytes))
            .is_err());

    StreamableHttpServerOptions duplicate_origins;
    duplicate_origins.allowed_origins = {"https://example.com", "https://example.com"};
    EXPECT_TRUE(StreamableHttpServer::create(make_session_factory(), std::move(duplicate_origins))
                    .is_err());
}

TEST(StreamableHttpServerTest, ModernPostWithoutSession)
{
    StreamableHttpServerOptions options;
    options.shared_server = make_shared_server();
    auto server           = TestServer::start(HttpSessionFactory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto response = send(client, server->url(), post_request(MODERN_TOOLS_LIST));
    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(response.headers.get("Content-Type"), "application/json");
    EXPECT_FALSE(response.headers.contains("MCP-Session-Id"));
    EXPECT_NE(body_text(response.body).find("resultType"), std::string::npos);
    EXPECT_EQ(server->session_count(), 0U);

    auto notified = send(
        client,
        server->url(),
        post_request(
            R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})"));
    EXPECT_EQ(notified.status, 202);
    EXPECT_EQ(server->session_count(), 0U);
}

TEST(StreamableHttpServerTest, DiscoverPostWithoutSession)
{
    StreamableHttpServerOptions options;
    options.shared_server = make_shared_server();
    auto server           = TestServer::start(HttpSessionFactory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto response = send(client,
                         server->url(),
                         post_request(R"({"jsonrpc":"2.0","id":3,"method":"server/discover"})"));
    ASSERT_EQ(response.status, 200);
    EXPECT_FALSE(response.headers.contains("MCP-Session-Id"));
    EXPECT_NE(body_text(response.body).find("supportedVersions"), std::string::npos);

    auto parsed = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_string_view(body_text(response.body)));
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_NE(std::move(parsed).unwrap().result(), nullptr);
}

TEST(StreamableHttpServerTest, InitializeStillCreatesSession)
{
    auto server = TestServer::start(make_session_factory());
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    ASSERT_EQ(initialized.status, 200);
    EXPECT_EQ(initialized.headers.get("Content-Type"), "application/json");
    const auto session_id = required_session_id(initialized);
    EXPECT_EQ(session_id.size(), 64U);
    EXPECT_EQ(server->session_count(), 1U);
}

TEST(StreamableHttpServerTest, HeaderMetaMismatch)
{
    StreamableHttpServerOptions options;
    options.shared_server = make_shared_server();
    auto server           = TestServer::start(HttpSessionFactory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto response = send(client,
                         server->url(),
                         post_request(MISMATCHED_META_TOOLS_LIST, std::nullopt, "2026-07-28"));
    EXPECT_EQ(json_rpc_error_code(response), -32020);
    EXPECT_FALSE(response.headers.contains("MCP-Session-Id"));
    EXPECT_EQ(server->session_count(), 0U);
}

TEST(StreamableHttpServerTest, HeaderOnlyVersionAccepted)
{
    StreamableHttpServerOptions options;
    options.shared_server = make_shared_server();
    auto server           = TestServer::start(HttpSessionFactory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto response = send(client,
                         server->url(),
                         post_request(R"({"jsonrpc":"2.0","id":4,"method":"ping"})",
                                      std::nullopt,
                                      "2026-07-28"));
    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(response.headers.get("Content-Type"), "application/json");
    auto parsed = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_string_view(body_text(response.body)));
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_NE(std::move(parsed).unwrap().result(), nullptr);
}

TEST(StreamableHttpServerTest, ModernRequestNotCountedAgainstSessionCap)
{
    StreamableHttpServerOptions options;
    options.max_sessions   = 1;
    options.shared_server  = make_shared_server();
    auto server            = TestServer::start(make_session_factory(), std::move(options));
    ASSERT_NE(server, nullptr);
    auto client = make_client();

    auto first = send(client, server->url(), post_request(MODERN_TOOLS_LIST));
    EXPECT_EQ(first.status, 200);
    auto second = send(client, server->url(), post_request(MODERN_TOOLS_LIST));
    EXPECT_EQ(second.status, 200);
    EXPECT_EQ(server->session_count(), 0U);

    auto initialized = send(client, server->url(), post_request(INITIALIZE));
    EXPECT_EQ(initialized.status, 200);
    EXPECT_FALSE(required_session_id(initialized).empty());
    EXPECT_EQ(server->session_count(), 1U);
}

}   // namespace
}   // namespace mcp::test
