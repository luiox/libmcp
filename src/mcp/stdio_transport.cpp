#include "mcp/stdio_transport.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace mcp {

StdioTransport::StdioTransport(ca::io::Reader& reader, ca::io::Writer& writer,
                               StdioTransportOptions options) noexcept
    : reader_(&reader)
    , writer_(&writer)
    , options_(options)
{}

StdioTransport::StdioTransport(StdioTransport&& other) noexcept
    : reader_(other.reader_)
    , writer_(other.writer_)
    , options_(other.options_)
    , buffer_(std::move(other.buffer_))
    , offset_(other.offset_)
    , eof_(other.eof_)
    , failed_(other.failed_)
{
    other.reader_ = nullptr;
    other.writer_ = nullptr;
    other.offset_ = 0;
    other.eof_    = false;
    other.failed_ = true;
}

StdioTransport& StdioTransport::operator=(StdioTransport&& other) noexcept
{
    if (this == &other) return *this;
    reader_  = other.reader_;
    writer_  = other.writer_;
    options_ = other.options_;
    buffer_  = std::move(other.buffer_);
    offset_  = other.offset_;
    eof_     = other.eof_;
    failed_  = other.failed_;

    other.reader_ = nullptr;
    other.writer_ = nullptr;
    other.offset_ = 0;
    other.eof_    = false;
    other.failed_ = true;
    return *this;
}

McpResult<StdioTransport> StdioTransport::create(ca::io::Reader& reader, ca::io::Writer& writer,
                                                 const StdioTransportOptions& options)
{
    if (options.max_message_bytes == 0)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::InvalidState, "stdio max_message_bytes must be greater than zero"));
    return ca::core::Ok(StdioTransport(reader, writer, options));
}

void StdioTransport::consume_prefix(ca::usize length)
{
    offset_ += length;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
        return;
    }
    if (offset_ >= 4096 && offset_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
}

McpResult<std::optional<JsonRpcMessage>> StdioTransport::read_message()
{
    if (reader_ == nullptr || failed_)
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState,
                                "stdio transport is moved from or framing is unrecoverable"));
    if (eof_) return ca::core::Ok(std::optional<JsonRpcMessage>{});

    std::array<ca::u8, 4096> input{};
    for (;;) {
        const auto begin   = buffer_.begin() + static_cast<std::ptrdiff_t>(offset_);
        const auto end     = buffer_.end();
        const auto newline = std::find(begin, end, static_cast<ca::u8>('\n'));
        if (newline != end) {
            const ca::usize line_length = static_cast<ca::usize>(newline - begin);
            const ca::usize consumed    = line_length + 1;
            if (line_length > options_.max_message_bytes) {
                consume_prefix(consumed);
                return ca::core::Err(
                    McpError::from_kind(McpErrorKind::MessageTooLarge,
                                        "stdio JSON-RPC message exceeds configured limit"));
            }

            const auto line = ca::str::Utf8StringRef::from_data(
                line_length == 0 ? nullptr : &*begin, line_length);
            auto parsed = JsonRpcMessage::parse(line);
            consume_prefix(consumed);
            if (parsed.is_err()) return ca::core::Err(std::move(parsed).unwrap_err());
            return ca::core::Ok(std::optional<JsonRpcMessage>(std::move(parsed).unwrap()));
        }

        if (buffer_.size() - offset_ > options_.max_message_bytes) {
            failed_ = true;
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::MessageTooLarge,
                "stdio JSON-RPC message exceeds configured limit before delimiter"));
        }

        auto read = reader_->read(input.data(), input.size());
        if (read.is_err()) {
            auto error = std::move(read).unwrap_err();
            if (error.kind() == ca::io::IoErrorKind::Interrupted) continue;
            return ca::core::Err(McpError::from_io(std::move(error), "read MCP stdio"));
        }
        const ca::usize count = read.unwrap();
        if (count > input.size()) {
            failed_ = true;
            return ca::core::Err(McpError::from_kind(
                McpErrorKind::InvalidState, "stdio Reader returned more bytes than requested"));
        }
        if (count == 0) {
            eof_ = true;
            if (buffer_.size() == offset_) {
                buffer_.clear();
                offset_ = 0;
                return ca::core::Ok(std::optional<JsonRpcMessage>{});
            }
            failed_ = true;
            return ca::core::Err(McpError::from_io(
                ca::io::IoError::from_kind(ca::io::IoErrorKind::UnexpectedEof,
                                           "stdio closed before JSON-RPC newline delimiter"),
                "read MCP stdio"));
        }
        buffer_.insert(buffer_.end(), input.begin(), input.begin() + count);
    }
}

McpResult<void> StdioTransport::write_message(const JsonRpcMessage& message)
{
    if (writer_ == nullptr)
        return ca::core::Err(
            McpError::from_kind(McpErrorKind::InvalidState, "stdio transport has been moved from"));

    auto encoded = message.serialize();
    if (encoded.byte_length() > options_.max_message_bytes)
        return ca::core::Err(McpError::from_kind(
            McpErrorKind::MessageTooLarge, "stdio JSON-RPC message exceeds configured limit"));
    auto body = writer_->write_all(encoded.data(), encoded.byte_length());
    if (body.is_err())
        return ca::core::Err(McpError::from_io(std::move(body).unwrap_err(), "write MCP stdio"));
    const ca::u8 newline = static_cast<ca::u8>('\n');
    auto         ending  = writer_->write_all(&newline, 1);
    if (ending.is_err())
        return ca::core::Err(McpError::from_io(std::move(ending).unwrap_err(), "write MCP stdio"));
    auto flushed = writer_->flush();
    if (flushed.is_err())
        return ca::core::Err(McpError::from_io(std::move(flushed).unwrap_err(), "flush MCP stdio"));
    return ca::core::Ok();
}

bool StdioTransport::eof() const noexcept
{
    return eof_;
}

}   // namespace mcp
