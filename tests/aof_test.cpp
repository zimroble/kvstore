#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "aof.h"
#include "commands.h"
#include "test_clock.h"

using kv::Aof;
using kv::CommandExecutor;
using kv::FsyncPolicy;
using kv::Store;

namespace {

std::string temp_path(const char* name) {
    return ::testing::TempDir() + "kv_" + name + "_" + std::to_string(::getpid()) + ".aof";
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs commands against a fresh store backed by `path`, then flushes.
void write_session(const std::string& path, FakeClock& clk, const std::vector<kv::resp::Command>& cmds) {
    Store store(clk.fn());
    Aof aof(path, FsyncPolicy::Always);
    CommandExecutor exec(store, &aof);
    aof.replay([&](const kv::resp::Command& c) { exec.replay(c); });
    ASSERT_TRUE(aof.open());
    std::string out;
    for (const auto& c : cmds) exec.execute(c, out);
    ASSERT_TRUE(aof.flush(clk.fn()()));
}

struct Loaded {
    std::unique_ptr<Store> store;
    kv::ReplayStats stats;
};

Loaded load(const std::string& path, FakeClock& clk) {
    Loaded l{std::make_unique<Store>(clk.fn()), {}};
    Aof aof(path, FsyncPolicy::Always);
    CommandExecutor exec(*l.store, &aof);
    l.stats = aof.replay([&](const kv::resp::Command& c) { exec.replay(c); });
    return l;
}

}  // namespace

TEST(Aof, ReplayRestoresState) {
    auto path = temp_path("replay");
    std::remove(path.c_str());
    FakeClock clk;
    write_session(path, clk, {{"SET", "a", "1"}, {"SET", "b", "2"}, {"INCR", "a"}, {"DEL", "b"},
                              {"APPEND", "a", "x"}, {"MSET", "c", "3", "d", "4"}});
    auto l = load(path, clk);
    ASSERT_TRUE(l.stats.ok);
    EXPECT_EQ(*l.store->get("a"), "2x");
    EXPECT_EQ(l.store->get("b"), nullptr);
    EXPECT_EQ(*l.store->get("d"), "4");
    std::remove(path.c_str());
}

TEST(Aof, ReadOnlyAndNoOpCommandsAreNotLogged) {
    auto path = temp_path("noop");
    std::remove(path.c_str());
    FakeClock clk;
    write_session(path, clk, {{"GET", "a"}, {"DEL", "missing"}, {"SET", "a", "1", "XX"}, {"PING"}});
    EXPECT_EQ(read_file(path), "");
    std::remove(path.c_str());
}

TEST(Aof, RelativeTtlsAreLoggedAsAbsoluteDeadlines) {
    auto path = temp_path("ttl");
    std::remove(path.c_str());
    FakeClock clk;
    write_session(path, clk, {{"SET", "k", "v", "EX", "10"}, {"SET", "j", "v"}, {"EXPIRE", "j", "100"}});

    clk.advance(5000);  // restart 5 s later: TTL keeps counting down
    auto l = load(path, clk);
    EXPECT_EQ(l.store->pttl("k"), 5000);
    EXPECT_EQ(l.store->pttl("j"), 95000);

    clk.advance(6000);  // past k's deadline: it must not come back
    auto l2 = load(path, clk);
    EXPECT_EQ(l2.store->get("k"), nullptr);
    EXPECT_NE(l2.store->get("j"), nullptr);
    std::remove(path.c_str());
}

TEST(Aof, TornTailIsTruncatedAndIgnored) {
    auto path = temp_path("torn");
    std::remove(path.c_str());
    FakeClock clk;
    write_session(path, clk, {{"SET", "a", "1"}, {"SET", "b", "2"}});
    auto good_size = read_file(path).size();
    {
        std::ofstream f(path, std::ios::app | std::ios::binary);
        f << "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$5\r\nhal";  // crash mid-write
    }
    auto l = load(path, clk);
    ASSERT_TRUE(l.stats.ok);
    EXPECT_EQ(l.stats.commands, 2u);
    EXPECT_GT(l.stats.truncated_bytes, 0u);
    EXPECT_EQ(l.store->get("c"), nullptr);
    EXPECT_EQ(read_file(path).size(), good_size);  // file repaired
    std::remove(path.c_str());
}

TEST(Aof, CorruptionInTheMiddleIsAnError) {
    auto path = temp_path("corrupt");
    FakeClock clk;
    {
        std::ofstream f(path, std::ios::binary);
        f << "*2\r\n$3\r\nDEL\r\n:1\r\n*1\r\n$4\r\nPING\r\n";
    }
    auto l = load(path, clk);
    EXPECT_FALSE(l.stats.ok);
    std::remove(path.c_str());
}

TEST(Aof, RewriteCompactsLogAndPreservesState) {
    auto path = temp_path("rewrite");
    std::remove(path.c_str());
    FakeClock clk;
    std::vector<kv::resp::Command> cmds;
    for (int i = 0; i < 1000; ++i) cmds.push_back({"INCR", "counter"});
    cmds.push_back({"SET", "temp", "x", "PX", "60000"});
    cmds.push_back({"SET", "gone", "x"});
    cmds.push_back({"DEL", "gone"});
    write_session(path, clk, cmds);
    auto before = read_file(path).size();

    {
        Store store(clk.fn());
        Aof aof(path, FsyncPolicy::Always);
        CommandExecutor exec(store, &aof);
        aof.replay([&](const kv::resp::Command& c) { exec.replay(c); });
        ASSERT_TRUE(aof.open());
        std::string out;
        exec.execute({"BGREWRITEAOF"}, out);
        EXPECT_EQ(out, "+OK\r\n");
        exec.execute({"SET", "after", "rewrite"}, out);  // appends go to the new file
        ASSERT_TRUE(aof.flush(clk.fn()()));
    }
    auto after = read_file(path).size();
    EXPECT_LT(after * 20, before);

    auto l = load(path, clk);
    EXPECT_EQ(*l.store->get("counter"), "1000");
    EXPECT_EQ(l.store->pttl("temp"), 60000);
    EXPECT_EQ(*l.store->get("after"), "rewrite");
    EXPECT_EQ(l.store->size(), 3u);
    std::remove(path.c_str());
}
