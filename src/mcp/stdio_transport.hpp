#pragma once

#include <vector>

#include <libca/io/reader.hpp>
#include <libca/io/writer.hpp>

#include "mcp/json_rpc.hpp"

namespace mcp {

/// @brief stdio 单条 JSON-RPC message 的 framing 限制。
struct StdioTransportOptions
{
    ca::usize max_message_bytes{4 * 1024 * 1024};   ///< 不含换行 delimiter 的最大 UTF-8 字节数。
};

/// @brief MCP newline-delimited stdio transport。
/// @details 本对象借用 Reader/Writer，不接管生命周期；二者地址必须在 transport 生命周期内
/// 保持有效。消息严格按一行一个 UTF-8 JSON-RPC object 处理。本类非线程安全。
class StdioTransport
{
public:
    StdioTransport(const StdioTransport&)            = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;
    StdioTransport(StdioTransport&& other) noexcept;
    StdioTransport& operator=(StdioTransport&& other) noexcept;
    ~StdioTransport() = default;

    /// @brief 校验限制并创建借用给定字节流的 transport。
    static McpResult<StdioTransport> create(
        ca::io::Reader& reader, ca::io::Writer& writer,
        const StdioTransportOptions& options = StdioTransportOptions());

    /// @brief 读取并校验下一条 JSON-RPC message；干净 EOF 返回空 optional。
    /// @note 超限且尚未找到换行时 framing 无法恢复，后续读取返回 InvalidState。
    McpResult<std::optional<JsonRpcMessage>> read_message();

    /// @brief 紧凑序列化 message，追加单个 LF 并 flush Writer。
    McpResult<void> write_message(const JsonRpcMessage& message);

    /// @brief 判断 Reader 是否已经返回干净 EOF。
    bool eof() const noexcept;

private:
    StdioTransport(ca::io::Reader& reader, ca::io::Writer& writer,
                   StdioTransportOptions options) noexcept;

    void consume_prefix(ca::usize length);

    ca::io::Reader*       reader_{nullptr};
    ca::io::Writer*       writer_{nullptr};
    StdioTransportOptions options_;
    std::vector<ca::u8>   buffer_;
    ca::usize             offset_{0};
    bool                  eof_{false};
    bool                  failed_{false};
};

}   // namespace mcp
