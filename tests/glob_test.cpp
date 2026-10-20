#include <gtest/gtest.h>

#include "commands.h"

using kv::glob_match;

TEST(Glob, Basics) {
    EXPECT_TRUE(glob_match("*", ""));
    EXPECT_TRUE(glob_match("*", "anything"));
    EXPECT_TRUE(glob_match("user:*", "user:42"));
    EXPECT_FALSE(glob_match("user:*", "session:42"));
    EXPECT_TRUE(glob_match("h?llo", "hello"));
    EXPECT_FALSE(glob_match("h?llo", "hllo"));
    EXPECT_TRUE(glob_match("*a*b*c", "xxaxxbxxc"));
    EXPECT_FALSE(glob_match("*a*b*c", "xxaxxcxxb"));
}

TEST(Glob, CharacterClasses) {
    EXPECT_TRUE(glob_match("h[ae]llo", "hallo"));
    EXPECT_FALSE(glob_match("h[ae]llo", "hillo"));
    EXPECT_TRUE(glob_match("h[^e]llo", "hallo"));
    EXPECT_FALSE(glob_match("h[^e]llo", "hello"));
    EXPECT_TRUE(glob_match("key[0-9]", "key7"));
    EXPECT_FALSE(glob_match("key[0-9]", "keyx"));
}

TEST(Glob, Escapes) {
    EXPECT_TRUE(glob_match("a\\*b", "a*b"));
    EXPECT_FALSE(glob_match("a\\*b", "axb"));
}

TEST(Glob, PathologicalPatternIsFast) {
    std::string s(5000, 'a');
    EXPECT_FALSE(glob_match("*a*a*a*a*a*a*a*a*a*b", s));
}
