#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
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

namespace mcp::test {
namespace {

constexpr std::string_view INITIALIZE =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"http-test","version":"1"}}})";

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

TEST(StreamableHttpServerTest, InitializesSessionAndHandlesBufferedMessages)
{
    auto server = TestServer::start(make_session);
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

TEST(StreamableHttpServerTest, ValidatesContentNegotiationAndOrigin)
{
    StreamableHttpServerOptions options;
    options.allowed_origins = {"https://allowed.example"};
    auto server             = TestServer::start(make_session, std::move(options));
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
    auto server = TestServer::start(make_session);
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
    auto server                             = TestServer::start(make_session, std::move(options));
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
    auto server = TestServer::start(make_session);
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
    auto server             = TestServer::start(make_session, std::move(options));
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

TEST(StreamableHttpServerTest, MapsMalformedAndInvalidMessagesToJsonRpcErrors)
{
    auto server = TestServer::start(make_session);
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
    auto failing = TestServer::start([]() -> McpResult<ServerSession> {
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState, "factory test failure"));
    });
    ASSERT_NE(failing, nullptr);
    auto failing_client = make_client();
    EXPECT_EQ(send(failing_client, failing->url(), post_request(INITIALIZE)).status, 500);

    StreamableHttpServerOptions throwing_options;
    throwing_options.max_sessions = 1;
    auto throwing                 = TestServer::start(
        []() -> McpResult<ServerSession> { throw std::runtime_error("factory failure"); },
        std::move(throwing_options));
    ASSERT_NE(throwing, nullptr);
    auto throwing_client = make_client();
    EXPECT_EQ(send(throwing_client, throwing->url(), post_request(INITIALIZE)).status, 500);
    EXPECT_EQ(send(throwing_client, throwing->url(), post_request(INITIALIZE)).status, 500);
    EXPECT_EQ(throwing->session_count(), 0U);

    StreamableHttpServerOptions options;
    options.max_sessions = 1;
    auto limited         = TestServer::start(make_session, std::move(options));
    ASSERT_NE(limited, nullptr);
    auto limited_client = make_client();
    EXPECT_EQ(send(limited_client, limited->url(), post_request(INITIALIZE)).status, 200);
    EXPECT_EQ(send(limited_client, limited->url(), post_request(INITIALIZE)).status, 503);
    EXPECT_EQ(limited->session_count(), 1U);
}

TEST(StreamableHttpServerTest, RemovesSessionAfterUnhandledFailure)
{
    auto server = TestServer::start([]() -> McpResult<ServerSession> {
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
    auto server = TestServer::start([state]() -> McpResult<ServerSession> {
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
    EXPECT_TRUE(StreamableHttpServer::create(make_session, std::move(invalid_endpoint)).is_err());

    StreamableHttpServerOptions invalid_capacity;
    invalid_capacity.max_sessions = 0;
    EXPECT_TRUE(StreamableHttpServer::create(make_session, std::move(invalid_capacity)).is_err());

    StreamableHttpServerOptions invalid_random;
    invalid_random.session_id_bytes = 1;
    EXPECT_TRUE(StreamableHttpServer::create(make_session, std::move(invalid_random)).is_err());

    StreamableHttpServerOptions duplicate_origins;
    duplicate_origins.allowed_origins = {"https://example.com", "https://example.com"};
    EXPECT_TRUE(StreamableHttpServer::create(make_session, std::move(duplicate_origins)).is_err());
}

}   // namespace
}   // namespace mcp::test
