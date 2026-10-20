#include <gtest/gtest.h>

#include "resp.h"

using namespace kv::resp;

TEST(Resp, ParsesArrayOfBulkStrings) {
    auto r = parse_command("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.command, (Command{"GET", "foo"}));
    EXPECT_EQ(r.consumed, 22u);
}

TEST(Resp, BinarySafeBulkStrings) {
    std::string payload("a\r\nb\0c", 6);
    std::string wire = "*1\r\n$6\r\n" + payload + "\r\n";
    auto r = parse_command(wire);
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.command[0], payload);
}

TEST(Resp, EmptyBulkString) {
    auto r = parse_command("*2\r\n$3\r\nSET\r\n$0\r\n\r\n");
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.command[1], "");
}

TEST(Resp, IncompleteAtEveryByteBoundary) {
    const std::string wire = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    for (std::size_t i = 0; i < wire.size(); ++i)
        EXPECT_EQ(parse_command(wire.substr(0, i)).status, ParseStatus::Incomplete) << "prefix " << i;
    EXPECT_EQ(parse_command(wire).status, ParseStatus::Ok);
}

TEST(Resp, PipelinedCommandsParseOneAtATime) {
    std::string wire = "*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n";
    auto a = parse_command(wire);
    ASSERT_EQ(a.status, ParseStatus::Ok);
    auto b = parse_command(std::string_view(wire).substr(a.consumed));
    ASSERT_EQ(b.status, ParseStatus::Ok);
    EXPECT_EQ(b.command, (Command{"ECHO", "hi"}));
    EXPECT_EQ(a.consumed + b.consumed, wire.size());
}

TEST(Resp, InlineCommands) {
    auto r = parse_command("SET  foo\tbar\r\n");
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.command, (Command{"SET", "foo", "bar"}));
    EXPECT_EQ(parse_command("PING").status, ParseStatus::Incomplete);
    EXPECT_EQ(parse_command("PING\n").command, (Command{"PING"}));
}

TEST(Resp, RejectsMalformedInput) {
    EXPECT_EQ(parse_command("*x\r\n").status, ParseStatus::Error);
    EXPECT_EQ(parse_command("*1\r\n:5\r\n").status, ParseStatus::Error);
    EXPECT_EQ(parse_command("*1\r\n$-5\r\n").status, ParseStatus::Error);
    EXPECT_EQ(parse_command("*1\r\n$3\r\nfooXX").status, ParseStatus::Error);
    EXPECT_EQ(parse_command("*99999999999\r\n").status, ParseStatus::Error);
}

TEST(Resp, EncodersProduceRedisWireFormat) {
    std::string out;
    simple_string(out, "OK");
    error(out, "ERR bad");
    integer(out, -42);
    bulk_string(out, "hey");
    null_bulk(out);
    array_header(out, 2);
    EXPECT_EQ(out, "+OK\r\n-ERR bad\r\n:-42\r\n$3\r\nhey\r\n$-1\r\n*2\r\n");
}

TEST(Resp, EncodeCommandRoundTrips) {
    Command cmd{"SET", "k", std::string("v\r\n\0", 4)};
    auto r = parse_command(encode_command(cmd));
    ASSERT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.command, cmd);
}
