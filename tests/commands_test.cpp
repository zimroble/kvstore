#include <gtest/gtest.h>

#include "commands.h"
#include "test_clock.h"

using kv::CommandExecutor;
using kv::Store;
using kv::resp::Command;

class Commands : public ::testing::Test {
protected:
    FakeClock clk;
    Store store{clk.fn()};
    CommandExecutor exec{store, nullptr};

    std::string run(Command cmd) {
        std::string out;
        exec.execute(cmd, out);
        return out;
    }
};

TEST_F(Commands, PingEcho) {
    EXPECT_EQ(run({"PING"}), "+PONG\r\n");
    EXPECT_EQ(run({"ping", "hi"}), "$2\r\nhi\r\n");
    EXPECT_EQ(run({"ECHO", "x"}), "$1\r\nx\r\n");
}

TEST_F(Commands, SetGet) {
    EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
    EXPECT_EQ(run({"SET", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
}

TEST_F(Commands, SetNxXx) {
    EXPECT_EQ(run({"SET", "k", "1", "XX"}), "$-1\r\n");
    EXPECT_EQ(run({"SET", "k", "1", "NX"}), "+OK\r\n");
    EXPECT_EQ(run({"SET", "k", "2", "NX"}), "$-1\r\n");
    EXPECT_EQ(run({"SET", "k", "3", "XX"}), "+OK\r\n");
    EXPECT_EQ(run({"GET", "k"}), "$1\r\n3\r\n");
    EXPECT_EQ(run({"SET", "k", "4", "NX", "XX"}), "-ERR syntax error\r\n");
}

TEST_F(Commands, SetGetOption) {
    EXPECT_EQ(run({"SET", "k", "a", "GET"}), "$-1\r\n");
    EXPECT_EQ(run({"SET", "k", "b", "GET"}), "$1\r\na\r\n");
}

TEST_F(Commands, SetWithExpiry) {
    EXPECT_EQ(run({"SET", "k", "v", "EX", "10"}), "+OK\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":10\r\n");
    EXPECT_EQ(run({"PTTL", "k"}), ":10000\r\n");
    clk.advance(10000);
    EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-2\r\n");

    EXPECT_EQ(run({"SET", "k", "v", "PX", "0"}), "-ERR invalid expire time in 'set' command\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EX", "abc"}), "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EX", "1", "PX", "1"}), "-ERR syntax error\r\n");
}

TEST_F(Commands, KeepTtl) {
    run({"SET", "k", "v", "PX", "5000"});
    run({"SET", "k", "w", "KEEPTTL"});
    EXPECT_EQ(run({"PTTL", "k"}), ":5000\r\n");
    run({"SET", "k", "z"});
    EXPECT_EQ(run({"PTTL", "k"}), ":-1\r\n");
}

TEST_F(Commands, IncrDecr) {
    EXPECT_EQ(run({"INCR", "n"}), ":1\r\n");
    EXPECT_EQ(run({"INCRBY", "n", "41"}), ":42\r\n");
    EXPECT_EQ(run({"DECR", "n"}), ":41\r\n");
    EXPECT_EQ(run({"DECRBY", "n", "50"}), ":-9\r\n");
    run({"SET", "s", "abc"});
    EXPECT_EQ(run({"INCR", "s"}), "-ERR value is not an integer or out of range\r\n");
    run({"SET", "big", "9223372036854775807"});
    EXPECT_EQ(run({"INCR", "big"}), "-ERR increment or decrement would overflow\r\n");
}

TEST_F(Commands, IncrPreservesTtl) {
    run({"SET", "n", "1", "PX", "3000"});
    run({"INCR", "n"});
    EXPECT_EQ(run({"PTTL", "n"}), ":3000\r\n");
}

TEST_F(Commands, MsetMgetAppendStrlen) {
    EXPECT_EQ(run({"MSET", "a", "1", "b", "2"}), "+OK\r\n");
    EXPECT_EQ(run({"MGET", "a", "missing", "b"}), "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n");
    EXPECT_EQ(run({"APPEND", "a", "23"}), ":3\r\n");
    EXPECT_EQ(run({"APPEND", "new", "xy"}), ":2\r\n");
    EXPECT_EQ(run({"STRLEN", "a"}), ":3\r\n");
    EXPECT_EQ(run({"STRLEN", "nope"}), ":0\r\n");
    EXPECT_EQ(run({"MSET", "a"}), "-ERR wrong number of arguments for 'mset' command\r\n");
}

TEST_F(Commands, DelExistsKeysDbsize) {
    run({"MSET", "user:1", "a", "user:2", "b", "post:1", "c"});
    EXPECT_EQ(run({"EXISTS", "user:1", "user:2", "user:3"}), ":2\r\n");
    EXPECT_EQ(run({"KEYS", "user:*"}), "*2\r\n$6\r\nuser:1\r\n$6\r\nuser:2\r\n");
    EXPECT_EQ(run({"DEL", "user:1", "post:1", "nope"}), ":2\r\n");
    EXPECT_EQ(run({"DBSIZE"}), ":1\r\n");
    EXPECT_EQ(run({"FLUSHALL"}), "+OK\r\n");
    EXPECT_EQ(run({"DBSIZE"}), ":0\r\n");
}

TEST_F(Commands, ExpireFamily) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "missing", "10"}), ":0\r\n");
    EXPECT_EQ(run({"EXPIRE", "k", "10"}), ":1\r\n");
    EXPECT_EQ(run({"PEXPIRE", "k", "2500"}), ":1\r\n");
    EXPECT_EQ(run({"PTTL", "k"}), ":2500\r\n");
    EXPECT_EQ(run({"PERSIST", "k"}), ":1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
    int64_t at = (*clk.now / 1000) + 100;
    EXPECT_EQ(run({"EXPIREAT", "k", std::to_string(at)}), ":1\r\n");
    EXPECT_EQ(run({"EXPIRE", "k", "-1"}), ":1\r\n");  // negative TTL deletes
    EXPECT_EQ(run({"EXISTS", "k"}), ":0\r\n");
}

TEST_F(Commands, ErrorsAreReported) {
    EXPECT_EQ(run({"NOPE"}), "-ERR unknown command 'NOPE'\r\n");
    EXPECT_EQ(run({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(run({"SELECT", "1"}), "-ERR DB index is out of range\r\n");
}

TEST_F(Commands, QuitClosesConnection) {
    std::string out;
    EXPECT_FALSE(exec.execute({"QUIT"}, out));
    EXPECT_EQ(out, "+OK\r\n");
}
