#include "mcp/streamable_http_server.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libca/crypto/hex.hpp>
#include <libca/crypto/random.hpp>
#include <libca/http/http_error.hpp>

namespace mcp {
namespace {

constexpr const char* SESSION_HEADER           = "MCP-Session-Id";
constexpr const char* PROTOCOL_VERSION_HEADER  = "MCP-Protocol-Version";
constexpr const char* LAST_EVENT_ID_HEADER     = "Last-Event-ID";
constexpr ca::i64     JSON_RPC_INVALID_REQUEST = -32600;
constexpr ca::i64     JSON_RPC_PARSE_ERROR     = -32700;
constexpr ca::usize   SESSION_ID_ATTEMPTS      = 8;

char ascii_lower(char value) noexcept
{
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value + ('a' - 'A'));
    return value;
}

bool ascii_equals(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (ca::usize index = 0; index < lhs.size(); ++index) {
        if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) return false;
    }
    return true;
}

std::string_view trim_ows(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool quality_allows(std::string_view value) noexcept
{
    value = trim_ows(value);
    if (value.empty() || (value.front() != '0' && value.front() != '1')) return false;
    if (value.size() == 1) return value.front() == '1';
    if (value[1] != '.' || value.size() > 5) return false;

    bool nonzero = value.front() == '1';
    for (ca::usize index = 2; index < value.size(); ++index) {
        const char digit = value[index];
        if (digit < '0' || digit > '9') return false;
        if (value.front() == '1' && digit != '0') return false;
        nonzero = nonzero || digit != '0';
    }
    return nonzero;
}

bool accept_item_allows(std::string_view item, std::string_view media_type) noexcept
{
    const auto separator = item.find(';');
    if (!ascii_equals(trim_ows(item.substr(0, separator)), media_type)) return false;
    if (separator == std::string_view::npos) return true;

    item.remove_prefix(separator + 1);
    bool saw_quality = false;
    while (!item.empty()) {
        const auto next      = item.find(';');
        const auto parameter = trim_ows(item.substr(0, next));
        const auto equals    = parameter.find('=');
        const auto name      = trim_ows(parameter.substr(0, equals));
        if (ascii_equals(name, "q")) {
            if (equals == std::string_view::npos || saw_quality ||
                !quality_allows(parameter.substr(equals + 1)))
                return false;
            saw_quality = true;
        }
        if (next == std::string_view::npos) break;
        item.remove_prefix(next + 1);
    }
    return true;
}

bool accepts(const ca::http::HttpHeaders& headers, std::string_view media_type)
{
    for (const auto value : headers.get_all("Accept")) {
        std::string_view remaining = value;
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            if (accept_item_allows(remaining.substr(0, comma), media_type)) return true;
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
    }
    return false;
}

bool is_json_content_type(const ca::http::HttpHeaders& headers)
{
    const auto values = headers.get_all("Content-Type");
    if (values.size() != 1) return false;
    const auto separator = values.front().find(';');
    return ascii_equals(trim_ows(values.front().substr(0, separator)), "application/json");
}

std::optional<std::string_view> single_header(const ca::http::HttpHeaders& headers,
                                              std::string_view name, bool& repeated)
{
    const auto values = headers.get_all(name);
    repeated          = values.size() > 1;
    if (values.size() != 1) return std::nullopt;
    return values.front();
}

ca::http::HttpResult<ca::http::HttpServerResponse> buffered_response(
    ca::u16 status, std::string_view body = {}, std::string_view content_type = {},
    std::optional<std::string_view> session_id = std::nullopt)
{
    ca::http::HttpResponse response;
    response.status = status;
    response.body =
        ca::core::Bytes::copy_from_slice(reinterpret_cast<const ca::u8*>(body.data()), body.size());
    if (!content_type.empty()) {
        auto appended = response.headers.append("Content-Type", std::string(content_type));
        if (appended.is_err()) return ca::core::Err(std::move(appended).unwrap_err());
    }
    if (session_id.has_value()) {
        auto appended = response.headers.append(SESSION_HEADER, std::string(*session_id));
        if (appended.is_err()) return ca::core::Err(std::move(appended).unwrap_err());
    }
    return ca::core::Ok(ca::http::HttpServerResponse::buffered(std::move(response)));
}

ca::http::HttpResult<ca::http::HttpServerResponse> text_response(ca::u16          status,
                                                                 std::string_view body)
{
    return buffered_response(status, body, "text/plain; charset=utf-8");
}

ca::http::HttpResult<ca::http::HttpServerResponse> json_response(
    ca::u16 status, const JsonRpcMessage& message,
    std::optional<std::string_view> session_id = std::nullopt)
{
    const auto encoded = message.serialize();
    return buffered_response(
        status,
        std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.byte_length()),
        "application/json",
        session_id);
}

ca::http::HttpResult<ca::http::HttpServerResponse> json_rpc_error(ca::u16 status, ca::i64 code,
                                                                  const std::string& message)
{
    auto response = JsonRpcMessage::make_error(std::nullopt, code, message);
    if (response.is_err())
        return ca::core::Err(ca::http::HttpError::from_kind(
            ca::http::HttpErrorKind::InvalidState, "failed to build MCP JSON-RPC error response"));
    return json_response(status, std::move(response).unwrap());
}

std::string encode_sse_event(std::string_view id, std::string_view data)
{
    std::string encoded;
    encoded.reserve(id.size() + data.size() + 14);
    encoded += "id: ";
    encoded.append(id.data(), id.size());
    encoded += "\ndata:";
    if (!data.empty()) {
        encoded += ' ';
        encoded.append(data.data(), data.size());
    }
    encoded += "\n\n";
    return encoded;
}

bool valid_endpoint(const std::string& endpoint) noexcept
{
    if (endpoint.empty() || endpoint.front() != '/' || endpoint.find('?') != std::string::npos ||
        endpoint.find('#') != std::string::npos)
        return false;
    for (unsigned char character : endpoint) {
        if (character <= 0x20 || character == 0x7f) return false;
    }
    return true;
}

}   // namespace

HttpAuthorizationDecision::HttpAuthorizationDecision(std::string identity)
    : authorized_(true)
    , identity_(std::move(identity))
{}

HttpAuthorizationDecision::HttpAuthorizationDecision(ca::http::HttpServerResponse response)
    : rejection_(std::move(response))
{}

HttpAuthorizationDecision HttpAuthorizationDecision::authorized(std::string identity)
{
    return HttpAuthorizationDecision(std::move(identity));
}

HttpAuthorizationDecision HttpAuthorizationDecision::rejected(ca::http::HttpServerResponse response)
{
    return HttpAuthorizationDecision(std::move(response));
}

bool HttpAuthorizationDecision::is_authorized() const noexcept
{
    return authorized_;
}

const std::string& HttpAuthorizationDecision::identity() const noexcept
{
    return identity_;
}

ca::http::HttpServerResponse HttpAuthorizationDecision::take_rejection()
{
    auto response = std::move(rejection_.value());
    rejection_.reset();
    return response;
}

class StreamableHttpServer::Impl
{
public:
    Impl(HttpSessionFactory factory, StreamableHttpServerOptions options)
        : factory_(std::move(factory))
        , options_(std::move(options))
    {}

    ca::http::HttpResult<ca::http::HttpServerResponse> handle_post(
        const ca::http::HttpServerRequestContext& context)
    {
        std::string authorization_identity;
        auto        authorization = authorize_request(context, authorization_identity);
        if (authorization.is_err()) return ca::core::Err(std::move(authorization).unwrap_err());
        auto rejection = std::move(authorization).unwrap();
        if (rejection.has_value()) return ca::core::Ok(std::move(*rejection));

        const auto& request = context.request();
        if (!is_json_content_type(request.headers))
            return text_response(415, "Content-Type must be application/json\n");
        if (!accepts(request.headers, "application/json") ||
            !accepts(request.headers, "text/event-stream"))
            return text_response(406,
                                 "Accept must include application/json and text/event-stream\n");

        auto parsed = JsonRpcMessage::parse(
            ca::str::Utf8StringRef::from_data(request.body.as_ptr(), request.body.remaining()));
        if (parsed.is_err()) {
            auto       error = std::move(parsed).unwrap_err();
            const auto code  = error.kind() == McpErrorKind::InvalidJson ? JSON_RPC_PARSE_ERROR
                                                                         : JSON_RPC_INVALID_REQUEST;
            return json_rpc_error(400, code, error.message());
        }
        auto message = std::move(parsed).unwrap();

        bool       repeated_session = false;
        auto       session_id = single_header(request.headers, SESSION_HEADER, repeated_session);
        const bool initialize = message.kind() == JsonRpcMessageKind::Request &&
                                message.method().has_value() && *message.method() == "initialize";
        if (!session_id.has_value() && !repeated_session && initialize)
            return initialize_session(message, context, std::move(authorization_identity));
        if (repeated_session) return text_response(400, "MCP-Session-Id must occur exactly once\n");
        if (!session_id.has_value() || session_id->empty())
            return text_response(400, "MCP-Session-Id is required\n");

        auto record = find_session(*session_id);
        if (record == nullptr) return text_response(404, "MCP session not found\n");

        std::unique_lock<std::mutex> lock(record->mutex);
        if (!record->active || record->authorization_identity != authorization_identity)
            return text_response(404, "MCP session not found\n");
        auto version_error = validate_protocol_version(request.headers, record->session);
        if (version_error.has_value()) return text_response(400, *version_error);

        auto handled = handle_session(record->session, message);
        if (handled.is_err()) {
            record->active = false;
            lock.unlock();
            remove_session(*session_id, record);
            return text_response(500, "MCP session failed\n");
        }
        auto response = std::move(handled).unwrap();
        if (!response.has_value()) return buffered_response(202);
        return message_response(*record, *response, request.version);
    }

    ca::http::HttpResult<ca::http::HttpServerResponse> handle_get(
        const ca::http::HttpServerRequestContext& context)
    {
        std::string authorization_identity;
        auto        authorization = authorize_request(context, authorization_identity);
        if (authorization.is_err()) return ca::core::Err(std::move(authorization).unwrap_err());
        auto rejection = std::move(authorization).unwrap();
        if (rejection.has_value()) return ca::core::Ok(std::move(*rejection));

        if (!options_.sse.has_value()) return get_not_supported_response();

        const auto& request           = context.request();
        bool        repeated_event_id = false;
        auto        last_event_id =
            single_header(request.headers, LAST_EVENT_ID_HEADER, repeated_event_id);
        if (repeated_event_id) return text_response(400, "Last-Event-ID must occur exactly once\n");
        if (!last_event_id.has_value()) return get_not_supported_response();
        if (last_event_id->empty()) return text_response(400, "Last-Event-ID must not be empty\n");
        if (!accepts(request.headers, "text/event-stream"))
            return text_response(406, "Accept must include text/event-stream\n");

        bool repeated_session = false;
        auto session_id       = single_header(request.headers, SESSION_HEADER, repeated_session);
        if (repeated_session) return text_response(400, "MCP-Session-Id must occur exactly once\n");
        if (!session_id.has_value() || session_id->empty())
            return text_response(400, "MCP-Session-Id is required\n");

        auto record = find_session(*session_id);
        if (record == nullptr) return text_response(404, "MCP session not found\n");

        std::lock_guard<std::mutex> lock(record->mutex);
        if (!record->active || record->authorization_identity != authorization_identity)
            return text_response(404, "MCP session not found\n");
        auto version_error = validate_protocol_version(request.headers, record->session);
        if (version_error.has_value()) return text_response(400, *version_error);

        auto cursor = find_replay_cursor(*record, *last_event_id);
        if (!cursor.has_value()) return text_response(404, "MCP SSE stream not found\n");
        return build_sse_response(cursor->stream, cursor->first_event);
    }

    ca::http::HttpResult<ca::http::HttpServerResponse> handle_delete(
        const ca::http::HttpServerRequestContext& context)
    {
        std::string authorization_identity;
        auto        authorization = authorize_request(context, authorization_identity);
        if (authorization.is_err()) return ca::core::Err(std::move(authorization).unwrap_err());
        auto rejection = std::move(authorization).unwrap();
        if (rejection.has_value()) return ca::core::Ok(std::move(*rejection));

        const auto& request          = context.request();
        bool        repeated_session = false;
        auto        session_id = single_header(request.headers, SESSION_HEADER, repeated_session);
        if (repeated_session) return text_response(400, "MCP-Session-Id must occur exactly once\n");
        if (!session_id.has_value() || session_id->empty())
            return text_response(400, "MCP-Session-Id is required\n");

        auto record = find_session(*session_id);
        if (record == nullptr) return text_response(404, "MCP session not found\n");
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            if (!record->active || record->authorization_identity != authorization_identity)
                return text_response(404, "MCP session not found\n");
            auto version_error = validate_protocol_version(request.headers, record->session);
            if (version_error.has_value()) return text_response(400, *version_error);
            record->active = false;
        }
        remove_session(*session_id, record);
        return buffered_response(204);
    }

    ca::usize session_count() const noexcept
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return sessions_.size();
    }

    const std::string& endpoint() const noexcept { return options_.endpoint; }

    ca::http::HttpResult<std::optional<ca::http::HttpServerResponse>> check_origin(
        const ca::http::HttpServerRequestContext& context)
    {
        if (!matches_endpoint(context.request().target) ||
            origin_allowed(context.request().headers))
            return ca::core::Ok(std::optional<ca::http::HttpServerResponse>{});

        auto response = text_response(403, "Forbidden\n");
        if (response.is_err()) return ca::core::Err(std::move(response).unwrap_err());
        return ca::core::Ok(
            std::optional<ca::http::HttpServerResponse>(std::move(response).unwrap()));
    }

private:
    struct SseEvent
    {
        std::string id;
        std::string encoded;
    };

    struct SseReplayStream
    {
        std::vector<SseEvent> events;
        ca::usize             encoded_bytes{0};
    };

    struct SseReplayCursor
    {
        std::shared_ptr<const SseReplayStream> stream;
        ca::usize                              first_event{0};
    };

    struct SessionRecord
    {
        SessionRecord(ServerSession value, std::string identity)
            : session(std::move(value))
            , authorization_identity(std::move(identity))
        {}

        std::mutex                                         mutex;
        ServerSession                                      session;
        std::string                                        authorization_identity;
        std::deque<std::shared_ptr<const SseReplayStream>> replay_streams;
        ca::usize                                          replay_bytes{0};
        ca::u64                                            next_stream_id{1};
        bool                                               active{true};
    };

    ca::http::HttpResult<ca::http::HttpServerResponse> get_not_supported_response() const
    {
        ca::http::HttpResponse response;
        response.status   = 405;
        auto content_type = response.headers.append("Content-Type", "text/plain; charset=utf-8");
        if (content_type.is_err()) return ca::core::Err(std::move(content_type).unwrap_err());
        auto allow = response.headers.append("Allow", "POST, DELETE");
        if (allow.is_err()) return ca::core::Err(std::move(allow).unwrap_err());
        constexpr std::string_view body = "Standalone SSE stream is not supported\n";
        response.body                   = ca::core::Bytes::copy_from_slice(
            reinterpret_cast<const ca::u8*>(body.data()), body.size());
        return ca::core::Ok(ca::http::HttpServerResponse::buffered(std::move(response)));
    }

    ca::http::HttpResult<ca::http::HttpServerResponse> build_sse_response(
        std::shared_ptr<const SseReplayStream> stream, ca::usize first_event,
        std::optional<std::string_view> session_id = std::nullopt) const
    {
        ca::http::HttpResponseHead response;
        response.status   = 200;
        auto content_type = response.headers.append("Content-Type", "text/event-stream");
        if (content_type.is_err()) return ca::core::Err(std::move(content_type).unwrap_err());
        auto cache_control = response.headers.append("Cache-Control", "no-cache");
        if (cache_control.is_err()) return ca::core::Err(std::move(cache_control).unwrap_err());
        if (session_id.has_value()) {
            auto appended = response.headers.append(SESSION_HEADER, std::string(*session_id));
            if (appended.is_err()) return ca::core::Err(std::move(appended).unwrap_err());
        }

        auto producer = [stream = std::move(stream), first_event](
                            ca::http::Http1ChunkedBodyWriter& writer,
                            const ca::thread::StopToken& stop_token) -> ca::http::HttpResult<void> {
            for (ca::usize index = first_event; index < stream->events.size(); ++index) {
                if (stop_token.stop_requested())
                    return ca::core::Err(ca::http::HttpError::from_kind(
                        ca::http::HttpErrorKind::InvalidState, "MCP SSE response was stopped"));
                auto written = writer.write_chunk(stream->events[index].encoded);
                if (written.is_err()) return ca::core::Err(std::move(written).unwrap_err());
                auto flushed = writer.flush();
                if (flushed.is_err()) return ca::core::Err(std::move(flushed).unwrap_err());
            }
            return ca::core::Ok();
        };
        return ca::core::Ok(
            ca::http::HttpServerResponse::chunked(std::move(response), std::move(producer)));
    }

    void remember_replay_stream(SessionRecord&                                record,
                                const std::shared_ptr<const SseReplayStream>& stream)
    {
        const auto& sse = *options_.sse;
        if (stream->encoded_bytes > sse.max_replay_bytes) return;
        while (!record.replay_streams.empty() &&
               (record.replay_streams.size() >= sse.max_replay_streams ||
                record.replay_bytes > sse.max_replay_bytes - stream->encoded_bytes)) {
            record.replay_bytes -= record.replay_streams.front()->encoded_bytes;
            record.replay_streams.pop_front();
        }
        record.replay_bytes += stream->encoded_bytes;
        record.replay_streams.push_back(stream);
    }

    ca::http::HttpResult<ca::http::HttpServerResponse> message_response(
        SessionRecord& record, const JsonRpcMessage& message, ca::http::HttpVersion version,
        std::optional<std::string_view> session_id = std::nullopt)
    {
        if (!options_.sse.has_value() || version != ca::http::HttpVersion::Http11)
            return json_response(200, message, session_id);
        if (record.next_stream_id == 0)
            return ca::core::Err(ca::http::HttpError::from_kind(
                ca::http::HttpErrorKind::InvalidState, "MCP SSE stream id space is exhausted"));

        const auto encoded_message = message.serialize();
        const auto message_view    = std::string_view(
            reinterpret_cast<const char*>(encoded_message.data()), encoded_message.byte_length());
        const auto stream_id = std::to_string(record.next_stream_id++);
        auto       stream    = std::make_shared<SseReplayStream>();
        stream->events.reserve(2);
        stream->events.push_back(
            SseEvent{stream_id + ":0", encode_sse_event(stream_id + ":0", {})});
        stream->events.push_back(
            SseEvent{stream_id + ":1", encode_sse_event(stream_id + ":1", message_view)});
        for (const auto& event : stream->events) stream->encoded_bytes += event.encoded.size();

        remember_replay_stream(record, stream);
        return build_sse_response(std::move(stream), 0, session_id);
    }

    std::optional<SseReplayCursor> find_replay_cursor(const SessionRecord& record,
                                                      std::string_view     last_event_id) const
    {
        for (const auto& stream : record.replay_streams) {
            for (ca::usize index = 0; index < stream->events.size(); ++index) {
                if (stream->events[index].id == last_event_id)
                    return SseReplayCursor{stream, index + 1};
            }
        }
        return std::nullopt;
    }

    ca::http::HttpResult<std::optional<ca::http::HttpServerResponse>> authorize_request(
        const ca::http::HttpServerRequestContext& context, std::string& identity)
    {
        identity.clear();
        if (!options_.authorizer)
            return ca::core::Ok(std::optional<ca::http::HttpServerResponse>{});

        auto authorized = invoke_authorizer(context);
        if (authorized.is_err()) return ca::core::Err(std::move(authorized).unwrap_err());
        auto decision = std::move(authorized).unwrap();
        if (!decision.is_authorized())
            return ca::core::Ok(
                std::optional<ca::http::HttpServerResponse>(decision.take_rejection()));
        if (decision.identity().empty())
            return ca::core::Err(
                ca::http::HttpError::from_kind(ca::http::HttpErrorKind::InvalidState,
                                               "MCP HTTP authorizer returned an empty identity"));
        identity = decision.identity();
        return ca::core::Ok(std::optional<ca::http::HttpServerResponse>{});
    }

    bool origin_allowed(const ca::http::HttpHeaders& headers) const
    {
        const auto origins = headers.get_all("Origin");
        if (origins.empty()) return true;
        if (origins.size() != 1) return false;
        return std::find(options_.allowed_origins.begin(),
                         options_.allowed_origins.end(),
                         origins.front()) != options_.allowed_origins.end();
    }

    bool matches_endpoint(std::string_view target) const noexcept
    {
        const auto limit = target.find_first_of("?#");
        return target.substr(0, limit) == options_.endpoint;
    }

    std::optional<std::string> validate_protocol_version(const ca::http::HttpHeaders& headers,
                                                         const ServerSession&         session) const
    {
        bool repeated = false;
        auto version  = single_header(headers, PROTOCOL_VERSION_HEADER, repeated);
        if (repeated) return std::string("MCP-Protocol-Version must occur exactly once\n");
        if (!version.has_value()) {
            if (options_.require_protocol_version_header)
                return std::string("MCP-Protocol-Version is required\n");
            return std::nullopt;
        }
        if (*version != session.negotiated_protocol_version())
            return std::string("MCP-Protocol-Version does not match the session\n");
        return std::nullopt;
    }

    std::shared_ptr<SessionRecord> find_session(std::string_view session_id) const
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto                  found = sessions_.find(std::string(session_id));
        return found == sessions_.end() ? nullptr : found->second;
    }

    void remove_session(std::string_view session_id, const std::shared_ptr<SessionRecord>& record)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto                        found = sessions_.find(std::string(session_id));
        if (found != sessions_.end() && found->second == record) sessions_.erase(found);
    }

    bool reserve_session()
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (sessions_.size() + pending_sessions_ >= options_.max_sessions) return false;
        ++pending_sessions_;
        return true;
    }

    void release_reservation() noexcept
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        --pending_sessions_;
    }

    ca::http::HttpResult<ca::http::HttpServerResponse> initialize_session(
        const JsonRpcMessage& message, const ca::http::HttpServerRequestContext& context,
        std::string authorization_identity)
    {
        if (!reserve_session()) return text_response(503, "MCP session capacity reached\n");

        const HttpSessionContext session_context{context, authorization_identity};
        auto                     created = create_session(session_context);
        if (created.is_err()) {
            release_reservation();
            return text_response(500, "MCP session factory failed\n");
        }
        auto session = std::move(created).unwrap();
        auto handled = handle_session(session, message);
        if (handled.is_err()) {
            release_reservation();
            return text_response(500, "MCP session failed\n");
        }
        auto response = std::move(handled).unwrap();
        if (!response.has_value()) {
            release_reservation();
            return text_response(500, "Initialize did not produce a response\n");
        }
        if (session.state() == ServerSessionState::AwaitingInitialize) {
            release_reservation();
            return json_response(200, *response);
        }

        auto record =
            std::make_shared<SessionRecord>(std::move(session), std::move(authorization_identity));
        for (ca::usize attempt = 0; attempt < SESSION_ID_ATTEMPTS; ++attempt) {
            auto random = ca::crypto::secure_random_bytes(options_.session_id_bytes);
            if (random.is_err()) {
                release_reservation();
                return text_response(500, "MCP session id generation failed\n");
            }
            auto       random_bytes = std::move(random).unwrap();
            const auto id           = ca::crypto::hex_encode(
                ca::core::ByteSlice(random_bytes.as_ptr(), random_bytes.remaining()));
            auto http_response =
                message_response(*record, *response, context.request().version, id);
            if (http_response.is_err()) {
                release_reservation();
                return http_response;
            }

            bool inserted = false;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                inserted = sessions_.emplace(id, record).second;
                if (inserted) --pending_sessions_;
            }
            if (inserted) return http_response;
        }
        release_reservation();
        return text_response(500, "MCP session id collision\n");
    }

    McpResult<ServerSession> create_session(const HttpSessionContext& context)
    {
        try {
            return factory_(context);
        }
        catch (const std::exception& error) {
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidState,
                std::string("MCP HTTP session factory threw an exception: ") + error.what()));
        }
        catch (...) {
            return ca::core::Err(
                McpError::from_kind(McpErrorKind::InvalidState,
                                    "MCP HTTP session factory threw a non-standard exception"));
        }
    }

    ca::http::HttpResult<HttpAuthorizationDecision> invoke_authorizer(
        const ca::http::HttpServerRequestContext& context)
    {
        try {
            return options_.authorizer(context);
        }
        catch (const std::exception& error) {
            return ca::core::Err(ca::http::HttpError::from_kind(
                ca::http::HttpErrorKind::InvalidState,
                std::string("MCP HTTP authorizer threw an exception: ") + error.what()));
        }
        catch (...) {
            return ca::core::Err(ca::http::HttpError::from_kind(
                ca::http::HttpErrorKind::InvalidState,
                "MCP HTTP authorizer threw a non-standard exception"));
        }
    }

    McpResult<std::optional<JsonRpcMessage>> handle_session(ServerSession&        session,
                                                            const JsonRpcMessage& message)
    {
        try {
            return session.handle(message);
        }
        catch (const std::exception& error) {
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidState,
                std::string("MCP HTTP session handler threw an exception: ") + error.what()));
        }
        catch (...) {
            return ca::core::Err(
                McpError::from_kind(McpErrorKind::InvalidState,
                                    "MCP HTTP session handler threw a non-standard exception"));
        }
    }

    HttpSessionFactory                                              factory_;
    StreamableHttpServerOptions                                     options_;
    mutable std::mutex                                              sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionRecord>> sessions_;
    ca::usize                                                       pending_sessions_{0};
};

StreamableHttpServer::StreamableHttpServer(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{}

McpResult<StreamableHttpServer> StreamableHttpServer::create(HttpSessionFactory          factory,
                                                             StreamableHttpServerOptions options)
{
    if (!factory)
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP HTTP session factory must not be empty"));
    if (!valid_endpoint(options.endpoint))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP HTTP endpoint must be an origin-form path"));
    if (options.max_sessions == 0)
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP HTTP max_sessions must be positive"));
    if (options.session_id_bytes < 16 || options.session_id_bytes > 64)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "MCP HTTP session_id_bytes must be between 16 and 64"));
    if (options.sse.has_value() &&
        (options.sse->max_replay_streams == 0 || options.sse->max_replay_bytes == 0))
        return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                 "MCP HTTP SSE replay limits must be positive"));
    for (ca::usize index = 0; index < options.allowed_origins.size(); ++index) {
        const auto& origin = options.allowed_origins[index];
        if (origin.empty() || !ca::http::HttpHeaders::valid_value(origin))
            return ca::core::Err(McpError::from_kind(McpErrorKind::InvalidState,
                                                     "MCP HTTP allowed Origin is invalid"));
        for (ca::usize previous = 0; previous < index; ++previous) {
            if (origin == options.allowed_origins[previous])
                return ca::core::Err(McpError::from_kind(
                    McpErrorKind::InvalidState, "MCP HTTP allowed Origins contain duplicates"));
        }
    }
    return ca::core::Ok(
        StreamableHttpServer(std::make_shared<Impl>(std::move(factory), std::move(options))));
}

ca::http::HttpResult<void> StreamableHttpServer::install(ca::http::HttpServer& server)
{
    if (impl_ == nullptr)
        return ca::core::Err(ca::http::HttpError::from_kind(
            ca::http::HttpErrorKind::InvalidState, "MCP HTTP adapter has been moved from"));
    auto impl   = impl_;
    auto origin = server.add_middleware([impl](const ca::http::HttpServerRequestContext& context) {
        return impl->check_origin(context);
    });
    if (origin.is_err()) return origin;
    auto post = server.route(
        "POST", impl->endpoint(), [impl](const ca::http::HttpServerRequestContext& context) {
            return impl->handle_post(context);
        });
    if (post.is_err()) return post;
    auto get = server.route(
        "GET", impl->endpoint(), [impl](const ca::http::HttpServerRequestContext& context) {
            return impl->handle_get(context);
        });
    if (get.is_err()) return get;
    return server.route(
        "DELETE", impl->endpoint(), [impl](const ca::http::HttpServerRequestContext& context) {
            return impl->handle_delete(context);
        });
}

ca::usize StreamableHttpServer::session_count() const noexcept
{
    return impl_ == nullptr ? 0 : impl_->session_count();
}

}   // namespace mcp
