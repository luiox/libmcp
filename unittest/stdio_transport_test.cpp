#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "mcp/stdio_transport.hpp"

namespace mcp::test {
namespace {

class StringReader final : public ca::io::Reader
{
public:
    StringReader(std::string data, ca::usize chunk_size)
        : data_(std::move(data))
        , chunk_size_(chunk_size)
    {}

    ca::io::IoResult<ca::usize> read(ca::u8* buffer, ca::usize capacity) override
    {
        if (position_ == data_.size()) return ca::core::Ok(static_cast<ca::usize>(0));
        const ca::usize count = std::min({capacity, chunk_size_, data_.size() - position_});
        std::memcpy(buffer, data_.data() + position_, count);
        position_ += count;
        return ca::core::Ok(count);
    }

private:
    std::string data_;
    ca::usize   chunk_size_{1};
    ca::usize   position_{0};
};

class StringWriter final : public ca::io::Writer
{
public:
    explicit StringWriter(ca::usize chunk_size = 3)
        : chunk_size_(chunk_size)
    {}

    ca::io::IoResult<ca::usize> write(const ca::u8* data, ca::usize length) override
    {
        const ca::usize count = std::min(length, chunk_size_);
        output_.append(reinterpret_cast<const char*>(data), count);
        return ca::core::Ok(count);
    }

    ca::io::IoResult<void> flush() override
    {
        ++flush_count_;
        return ca::core::Ok();
    }

    const std::string& output() const noexcept { return output_; }
    ca::usize          flush_count() const noexcept { return flush_count_; }

private:
    std::string output_;
    ca::usize   chunk_size_{3};
    ca::usize   flush_count_{0};
};

TEST(StdioTransportTest, ReadsFragmentedConsecutiveMessagesAndCleanEof)
{
    StringReader reader("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n"
                        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n",
                        2);
    StringWriter writer;
    auto         created = StdioTransport::create(reader, writer);
    ASSERT_TRUE(created.is_ok());
    auto transport = std::move(created).unwrap();

    auto first = transport.read_message();
    ASSERT_TRUE(first.is_ok());
    auto first_message = std::move(first).unwrap();
    ASSERT_TRUE(first_message.has_value());
    EXPECT_EQ(first_message->kind(), JsonRpcMessageKind::Request);

    auto second = transport.read_message();
    ASSERT_TRUE(second.is_ok());
    auto second_message = std::move(second).unwrap();
    ASSERT_TRUE(second_message.has_value());
    EXPECT_EQ(second_message->kind(), JsonRpcMessageKind::Notification);

    auto eof = transport.read_message();
    ASSERT_TRUE(eof.is_ok());
    auto eof_message = std::move(eof).unwrap();
    EXPECT_FALSE(eof_message.has_value());
    EXPECT_TRUE(transport.eof());
}

TEST(StdioTransportTest, WritesCompactJsonWithOneLfAndFlushes)
{
    StringReader reader("", 1);
    StringWriter writer(2);
    auto         created = StdioTransport::create(reader, writer);
    ASSERT_TRUE(created.is_ok());
    auto transport = std::move(created).unwrap();
    auto parsed    = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_cstr("{ \"jsonrpc\": \"2.0\", \"method\": \"ping\" }"));
    ASSERT_TRUE(parsed.is_ok());
    auto outbound = std::move(parsed).unwrap();

    auto written = transport.write_message(outbound);
    ASSERT_TRUE(written.is_ok()) << written.unwrap_err().to_string();
    EXPECT_EQ(writer.output(), "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}\n");
    EXPECT_EQ(writer.flush_count(), 1U);
}

TEST(StdioTransportTest, EnforcesWriteLimitBeforeWriting)
{
    StringReader          reader("", 1);
    StringWriter          writer;
    StdioTransportOptions options;
    options.max_message_bytes = 8;
    auto created              = StdioTransport::create(reader, writer, options);
    ASSERT_TRUE(created.is_ok());
    auto transport = std::move(created).unwrap();
    auto parsed    = JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_cstr("{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}"));
    ASSERT_TRUE(parsed.is_ok());
    auto message = std::move(parsed).unwrap();

    auto written = transport.write_message(message);
    ASSERT_TRUE(written.is_err());
    EXPECT_EQ(written.unwrap_err().kind(), McpErrorKind::MessageTooLarge);
    EXPECT_TRUE(writer.output().empty());
    EXPECT_EQ(writer.flush_count(), 0U);
}

TEST(StdioTransportTest, InvalidatesMovedFromTransport)
{
    StringReader reader("", 1);
    StringWriter writer;
    auto         created = StdioTransport::create(reader, writer);
    ASSERT_TRUE(created.is_ok());
    auto source    = std::move(created).unwrap();
    auto transport = std::move(source);

    auto moved_from_read = source.read_message();
    ASSERT_TRUE(moved_from_read.is_err());
    EXPECT_EQ(moved_from_read.unwrap_err().kind(), McpErrorKind::InvalidState);

    auto eof = transport.read_message();
    ASSERT_TRUE(eof.is_ok());
    EXPECT_FALSE(std::move(eof).unwrap().has_value());
}

TEST(StdioTransportTest, ConsumesInvalidLineAndCanReadNextMessage)
{
    StringReader reader("not-json\n{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}\n", 128);
    StringWriter writer;
    auto         created = StdioTransport::create(reader, writer);
    ASSERT_TRUE(created.is_ok());
    auto transport = std::move(created).unwrap();

    auto invalid = transport.read_message();
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(invalid.unwrap_err().kind(), McpErrorKind::InvalidJson);

    auto valid = transport.read_message();
    ASSERT_TRUE(valid.is_ok());
    auto valid_message = std::move(valid).unwrap();
    ASSERT_TRUE(valid_message.has_value());
    EXPECT_EQ(valid_message->kind(), JsonRpcMessageKind::Notification);
}

TEST(StdioTransportTest, EnforcesLimitAndDetectsTruncatedFrame)
{
    StdioTransportOptions options;
    options.max_message_bytes = 8;

    StringReader oversized("123456789", 9);
    StringWriter writer;
    auto         created = StdioTransport::create(oversized, writer, options);
    ASSERT_TRUE(created.is_ok());
    auto transport = std::move(created).unwrap();
    EXPECT_EQ(transport.read_message().unwrap_err().kind(), McpErrorKind::MessageTooLarge);
    EXPECT_EQ(transport.read_message().unwrap_err().kind(), McpErrorKind::InvalidState);

    StringReader truncated("{\"jsonrpc\":\"2.0\"}", 128);
    auto         truncated_created = StdioTransport::create(truncated, writer);
    ASSERT_TRUE(truncated_created.is_ok());
    auto truncated_transport = std::move(truncated_created).unwrap();
    auto failure             = truncated_transport.read_message();
    ASSERT_TRUE(failure.is_err());
    EXPECT_EQ(failure.unwrap_err().kind(), McpErrorKind::Io);
    ASSERT_NE(failure.unwrap_err().io_error(), nullptr);
    EXPECT_EQ(failure.unwrap_err().io_error()->kind(), ca::io::IoErrorKind::UnexpectedEof);
}

TEST(StdioTransportTest, RejectsZeroMessageLimit)
{
    StringReader          reader("", 1);
    StringWriter          writer;
    StdioTransportOptions options;
    options.max_message_bytes = 0;
    auto created              = StdioTransport::create(reader, writer, options);
    ASSERT_TRUE(created.is_err());
    EXPECT_EQ(created.unwrap_err().kind(), McpErrorKind::InvalidState);
}

}   // namespace
}   // namespace mcp::test
