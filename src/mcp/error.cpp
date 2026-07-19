#include "mcp/error.hpp"

#include <utility>

namespace mcp {

const char* mcp_error_kind_name(McpErrorKind kind) noexcept
{
    switch (kind) {
    case McpErrorKind::Io: return "Io";
    case McpErrorKind::InvalidJson: return "InvalidJson";
    case McpErrorKind::InvalidMessage: return "InvalidMessage";
    case McpErrorKind::MessageTooLarge: return "MessageTooLarge";
    case McpErrorKind::InvalidState: return "InvalidState";
    }
    return "Unknown";
}

McpError::McpError(McpErrorKind kind, std::string message,
                   std::optional<ca::io::IoError> io_error) noexcept
    : kind_(kind)
    , message_(std::move(message))
    , io_error_(std::move(io_error))
{}

McpError McpError::from_kind(McpErrorKind kind, std::string message)
{
    return McpError(kind, std::move(message), std::nullopt);
}

McpError McpError::from_io(ca::io::IoError error, std::string operation)
{
    std::string message = std::move(operation);
    if (!message.empty()) message += ": ";
    message += error.to_string();
    return McpError(McpErrorKind::Io, std::move(message), std::move(error));
}

McpErrorKind McpError::kind() const noexcept
{
    return kind_;
}

const std::string& McpError::message() const noexcept
{
    return message_;
}

const ca::io::IoError* McpError::io_error() const noexcept
{
    return io_error_.has_value() ? &*io_error_ : nullptr;
}

std::string McpError::to_string() const
{
    return std::string(mcp_error_kind_name(kind_)) + ": " + message_;
}

}   // namespace mcp
