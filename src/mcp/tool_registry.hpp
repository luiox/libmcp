#pragma once

#include <functional>
#include <string>
#include <vector>

#include <libca/json/json.hpp>

#include "mcp/server.hpp"

namespace mcp {

/// @brief 拥有并校验一条 MCP Tool descriptor。
/// @details 保留完整 JSON object，因此 title、icons、annotations、execution 及未来扩展字段
/// 都能原样出现在 tools/list 中。本类仅移动。
class ToolDefinition
{
public:
    ToolDefinition(const ToolDefinition&)                = delete;
    ToolDefinition& operator=(const ToolDefinition&)     = delete;
    ToolDefinition(ToolDefinition&&) noexcept            = default;
    ToolDefinition& operator=(ToolDefinition&&) noexcept = default;
    ~ToolDefinition()                                    = default;

    /// @brief 解析并校验 UTF-8 JSON Tool descriptor。
    static McpResult<ToolDefinition> parse(const ca::str::Utf8StringRef& input);

    /// @brief 接管并校验 Tool descriptor document。
    /// @note name 与 inputSchema 必填；inputSchema/outputSchema 必须是 object。
    static McpResult<ToolDefinition> from_document(ca::json::JsonDocument document);

    /// @brief 返回 tool name，生命周期绑定本对象。
    ca::str::Utf8StringRef name() const noexcept;

    /// @brief 返回完整 descriptor document。
    const ca::json::JsonDocument& document() const noexcept;

private:
    explicit ToolDefinition(ca::json::JsonDocument document) noexcept;

    ca::json::JsonDocument document_;
};

/// @brief 同步 tool handler；arguments 仅在回调期间有效。
/// @details 成功值必须是符合 CallToolResult 基础形态的 object document，至少包含 content array。
/// 工具执行本身的失败应使用 isError=true 的成功 result；MethodError 用于协议级参数或内部错误。
using ToolHandler = std::function<MethodResult(const ca::json::JsonValue& arguments)>;

/// @brief MCP tools/list 与 tools/call 的同步 registry。
/// @details registry 非线程安全。通过 ServerSession::install_tools 安装时 session 的 handler
/// 会持有 shared_ptr，确保回调期间 registry 存活。
class ToolRegistry
{
public:
    ToolRegistry()                               = default;
    ToolRegistry(const ToolRegistry&)            = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&)                 = delete;
    ToolRegistry& operator=(ToolRegistry&&)      = delete;
    ~ToolRegistry()                              = default;

    /// @brief 注册 tool；名称重复、descriptor、handler 无效或 registry 已安装时返回错误。
    McpResult<void> register_tool(ToolDefinition definition, ToolHandler handler);

    /// @brief 返回已注册 tool 数量。
    ca::usize size() const noexcept;

private:
    struct Entry
    {
        std::string    name;
        ToolDefinition definition;
        ToolHandler    handler;
    };

    MethodResult handle_list(const JsonRpcMessage& request) const;
    MethodResult handle_call(const JsonRpcMessage& request) const;
    void         freeze() noexcept;

    std::vector<Entry> entries_;
    bool               frozen_{false};

    friend class ServerSession;
};

}   // namespace mcp
