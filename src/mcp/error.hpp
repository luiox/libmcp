#pragma once

#include <optional>
#include <string>

#include <libca/core/result.hpp>
#include <libca/io/error.hpp>

namespace mcp {

/// @brief libmcp 协议、framing 与 IO 失败的稳定错误类别。
enum class McpErrorKind
{
    Io,                ///< 底层 Reader 或 Writer 失败。
    InvalidJson,       ///< 输入不是合法 UTF-8 JSON。
    InvalidMessage,    ///< JSON 不满足 MCP JSON-RPC message 约束。
    MessageTooLarge,   ///< transport message 超过配置上限。
    InvalidState       ///< 对移动后或 framing 已失步的对象继续操作。
};

/// @brief 返回稳定的 libmcp 错误类别名称。
const char* mcp_error_kind_name(McpErrorKind kind) noexcept;

/// @brief 保存稳定类别、诊断文本和可选底层 IO 错误。
class McpError
{
public:
    /// @brief 创建协议、限制或状态错误。
    static McpError from_kind(McpErrorKind kind, std::string message);

    /// @brief 包装底层 IO 错误并保留原始错误信息。
    static McpError from_io(ca::io::IoError error, std::string operation = {});

    /// @brief 返回稳定错误类别。
    McpErrorKind kind() const noexcept;

    /// @brief 返回人类可读诊断文本。
    const std::string& message() const noexcept;

    /// @brief 返回底层 IO 错误；非 IO 错误返回 nullptr。
    const ca::io::IoError* io_error() const noexcept;

    /// @brief 返回包含类别、消息和底层错误的诊断字符串。
    std::string to_string() const;

private:
    McpError(McpErrorKind kind, std::string message,
             std::optional<ca::io::IoError> io_error) noexcept;

    McpErrorKind                   kind_{McpErrorKind::InvalidMessage};
    std::string                    message_;
    std::optional<ca::io::IoError> io_error_;
};

/// @brief 使用 McpError 作为错误类型的 Result。
template<typename T>
using McpResult = ca::core::Result<T, McpError>;

}   // namespace mcp
