#include <gtest/gtest.h>

#include <random>
#include <string>
#include <unordered_map>

#include "store.h"
#include "test_clock.h"

using kv::Store;

TEST(Store, SetGetDelete) {
    Store s;
    EXPECT_EQ(s.get("a"), nullptr);
    s.set("a", "1");
    ASSERT_NE(s.get("a"), nullptr);
    EXPECT_EQ(*s.get("a"), "1");
    s.set("a", "2");
    EXPECT_EQ(*s.get("a"), "2");
    EXPECT_TRUE(s.del("a"));
    EXPECT_FALSE(s.del("a"));
    EXPECT_EQ(s.size(), 0u);
}

TEST(Store, LazyExpiryOnRead) {
    FakeClock clk;
    Store s(clk.fn());
    s.set("k", "v", clk.fn()() + 1000);
    EXPECT_EQ(s.pttl("k"), 1000);
    clk.advance(999);
    EXPECT_TRUE(s.exists("k"));
    clk.advance(1);
    EXPECT_FALSE(s.exists("k"));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.keys_with_ttl(), 0u);
}

TEST(Store, OverwriteClearsTtlUnlessKeepTtl) {
    FakeClock clk;
    Store s(clk.fn());
    const int64_t t0 = clk.fn()();
    s.set("k", "v", t0 + 500);
    s.set("k", "w", Store::kNoExpiry, /*keep_ttl=*/true);
    EXPECT_EQ(s.pttl("k"), 500);
    s.set("k", "x");
    EXPECT_EQ(s.pttl("k"), -1);
    EXPECT_EQ(s.keys_with_ttl(), 0u);
}

TEST(Store, ExpireAtInPastDeletes) {
    FakeClock clk;
    Store s(clk.fn());
    s.set("k", "v");
    EXPECT_TRUE(s.expire_at("k", clk.fn()() - 1));
    EXPECT_FALSE(s.exists("k"));
    EXPECT_FALSE(s.expire_at("missing", clk.fn()() + 10));
}

TEST(Store, PersistRemovesTtl) {
    FakeClock clk;
    Store s(clk.fn());
    s.set("k", "v", clk.fn()() + 100);
    EXPECT_TRUE(s.persist("k"));
    EXPECT_FALSE(s.persist("k"));
    clk.advance(1000);
    EXPECT_TRUE(s.exists("k"));
    EXPECT_EQ(s.pttl("missing"), -2);
}

TEST(Store, ActiveExpiryReclaimsUnreadKeys) {
    FakeClock clk;
    Store s(clk.fn());
    const int64_t t0 = clk.fn()();
    for (int i = 0; i < 10000; ++i) s.set("short:" + std::to_string(i), "x", t0 + 100);
    for (int i = 0; i < 1000; ++i) s.set("long:" + std::to_string(i), "x", t0 + 1'000'000);
    for (int i = 0; i < 1000; ++i) s.set("forever:" + std::to_string(i), "x");
    clk.advance(200);

    // Keys are never read; only the background cycle can remove them.
    std::size_t removed = 0;
    for (int cycle = 0; cycle < 10000 && s.size() > 2000; ++cycle) removed += s.active_expire_cycle();
    EXPECT_EQ(removed, 10000u);
    EXPECT_EQ(s.size(), 2000u);
    EXPECT_EQ(s.keys_with_ttl(), 1000u);
}

TEST(Store, ActiveExpiryStopsEarlyWhenFewKeysAreStale) {
    FakeClock clk;
    Store s(clk.fn());
    for (int i = 0; i < 1000; ++i) s.set(std::to_string(i), "x", clk.fn()() + 1'000'000);
    EXPECT_EQ(s.active_expire_cycle(), 0u);
}

// Randomised model check: apply a random op sequence to the Store and to a
// trivially-correct reference model, and compare after every step.
TEST(Store, MatchesReferenceModelUnderRandomOps) {
    FakeClock clk;
    Store s(clk.fn());
    struct Ref { std::string v; int64_t exp; };
    std::unordered_map<std::string, Ref> ref;
    std::mt19937 rng(1234);
    auto live = [&](const std::string& k) {
        auto it = ref.find(k);
        if (it == ref.end()) return false;
        if (it->second.exp != -1 && it->second.exp <= *clk.now) { ref.erase(it); return false; }
        return true;
    };

    for (int step = 0; step < 50000; ++step) {
        std::string k = "k" + std::to_string(rng() % 64);
        switch (rng() % 7) {
            case 0: case 1: {
                std::string v = std::to_string(rng());
                int64_t exp = (rng() % 3 == 0) ? *clk.now + 1 + rng() % 50 : -1;
                s.set(k, v, exp);
                ref[k] = {v, exp};
                break;
            }
            case 2: {
                bool expected = live(k);
                if (expected) ref.erase(k);
                ASSERT_EQ(s.del(k), expected);
                break;
            }
            case 3: {
                int64_t at = *clk.now + 1 + rng() % 50;
                bool expected = live(k);
                if (expected) ref[k].exp = at;
                ASSERT_EQ(s.expire_at(k, at), expected);
                break;
            }
            case 4: clk.advance(rng() % 10); break;
            case 5: s.active_expire_cycle(); break;
            case 6: {
                bool expected = live(k) && ref[k].exp != -1;
                if (expected) ref[k].exp = -1;
                ASSERT_EQ(s.persist(k), expected);
                break;
            }
        }
        bool expected = live(k);
        const std::string* got = s.get(k);
        ASSERT_EQ(got != nullptr, expected) << "step " << step;
        if (expected) {
            ASSERT_EQ(*got, ref[k].v);
        }
    }
    std::size_t live_count = 0;
    for (auto it = ref.begin(); it != ref.end(); ++it)
        if (it->second.exp == -1 || it->second.exp > *clk.now) ++live_count;
    std::size_t store_count = 0;
    s.for_each([&](const std::string&, const std::string&, int64_t) { ++store_count; });
    EXPECT_EQ(store_count, live_count);
}
