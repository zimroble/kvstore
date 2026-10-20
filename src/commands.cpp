#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <unordered_map>

namespace kv {
namespace {

std::string upper(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

bool to_int64(std::string_view s, long long& out) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

constexpr const char* kNotInt = "ERR value is not an integer or out of range";
constexpr const char* kSyntax = "ERR syntax error";
constexpr const char* kInvalidExpire = "ERR invalid expire time in 'set' command";

void wrong_args(std::string& out, const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    resp::error(out, "ERR wrong number of arguments for '" + lower + "' command");
}

bool class_match(std::string_view pattern, std::size_t& i, char c) {
    // pattern[i] is just past '['
    bool negate = false, matched = false;
    if (i < pattern.size() && pattern[i] == '^') { negate = true; ++i; }
    while (i < pattern.size() && pattern[i] != ']') {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            ++i;
            if (pattern[i] == c) matched = true;
        } else if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            char lo = pattern[i], hi = pattern[i + 2];
            if (lo > hi) std::swap(lo, hi);
            if (c >= lo && c <= hi) matched = true;
            i += 2;
        } else if (pattern[i] == c) {
            matched = true;
        }
        ++i;
    }
    if (i < pattern.size()) ++i;  // skip ']'
    return matched != negate;
}

}  // namespace

bool glob_match(std::string_view p, std::string_view s) {
    // Iterative matcher with single-star backtracking: O(|p| * |s|) worst case.
    std::size_t pi = 0, si = 0, star_p = std::string_view::npos, star_s = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star_p = ++pi;
            star_s = si;
            continue;
        }
        if (pi < p.size()) {
            std::size_t next = pi;
            bool ok;
            if (p[pi] == '?') {
                ok = true; next = pi + 1;
            } else if (p[pi] == '[') {
                next = pi + 1;
                ok = class_match(p, next, s[si]);
            } else if (p[pi] == '\\' && pi + 1 < p.size()) {
                ok = p[pi + 1] == s[si]; next = pi + 2;
            } else {
                ok = p[pi] == s[si]; next = pi + 1;
            }
            if (ok) { pi = next; ++si; continue; }
        }
        if (star_p == std::string_view::npos) return false;
        pi = star_p;
        si = ++star_s;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

void CommandExecutor::log(const resp::Command& cmd) {
    if (aof_ && !replaying_) aof_->append(cmd);
}

void CommandExecutor::replay(const resp::Command& cmd) {
    std::string sink;
    replaying_ = true;
    execute(cmd, sink);
    replaying_ = false;
}

bool CommandExecutor::execute(const resp::Command& cmd, std::string& out) {
    if (cmd.empty()) return true;
    if (stats_ && !replaying_) ++stats_->total_commands;
    const std::string name = upper(cmd[0]);
    const std::size_t argc = cmd.size();

    // ---- connection / server ------------------------------------------------
    if (name == "PING") {
        if (argc == 1) resp::simple_string(out, "PONG");
        else if (argc == 2) resp::bulk_string(out, cmd[1]);
        else wrong_args(out, name);
        return true;
    }
    if (name == "ECHO") {
        if (argc != 2) wrong_args(out, name);
        else resp::bulk_string(out, cmd[1]);
        return true;
    }
    if (name == "QUIT") {
        resp::simple_string(out, "OK");
        return false;
    }
    if (name == "SELECT") {
        if (argc != 2) wrong_args(out, name);
        else if (cmd[1] != "0") resp::error(out, "ERR DB index is out of range");
        else resp::simple_string(out, "OK");
        return true;
    }
    if (name == "COMMAND" || name == "CONFIG") {
        // Enough for redis-cli / redis-benchmark start-up probes.
        resp::array_header(out, 0);
        return true;
    }
    if (name == "DBSIZE") {
        resp::integer(out, static_cast<long long>(store_.size()));
        return true;
    }
    if (name == "INFO") {
        std::string info = "# Server\r\nkvstore_version:1.0.0\r\n# Keyspace\r\nkeys:" +
                           std::to_string(store_.size()) + "\r\nkeys_with_ttl:" +
                           std::to_string(store_.keys_with_ttl()) + "\r\n";
        if (stats_)
            info += "# Stats\r\nconnected_clients:" + std::to_string(stats_->connected_clients) +
                    "\r\ntotal_connections_received:" + std::to_string(stats_->total_connections) +
                    "\r\ntotal_commands_processed:" + std::to_string(stats_->total_commands) +
                    "\r\nexpired_keys:" + std::to_string(stats_->expired_keys) + "\r\n";
        resp::bulk_string(out, info);
        return true;
    }
    if (name == "FLUSHALL" || name == "FLUSHDB") {
        store_.clear();
        log({"FLUSHALL"});
        resp::simple_string(out, "OK");
        return true;
    }
    if (name == "BGREWRITEAOF" || name == "REWRITEAOF") {
        if (!aof_) resp::error(out, "ERR AOF is disabled");
        else if (!aof_->rewrite(store_)) resp::error(out, "ERR AOF rewrite failed");
        else resp::simple_string(out, "OK");
        return true;
    }

    // ---- strings ------------------------------------------------------------
    if (name == "GET") {
        if (argc != 2) { wrong_args(out, name); return true; }
        const std::string* v = store_.get(cmd[1]);
        v ? resp::bulk_string(out, *v) : resp::null_bulk(out);
        return true;
    }
    if (name == "SET") {
        if (argc < 3) { wrong_args(out, name); return true; }
        bool nx = false, xx = false, keep_ttl = false, get = false;
        int64_t expire_at = Store::kNoExpiry;
        int ttl_opts = 0;
        for (std::size_t i = 3; i < argc; ++i) {
            const std::string opt = upper(cmd[i]);
            if (opt == "NX") nx = true;
            else if (opt == "XX") xx = true;
            else if (opt == "GET") get = true;
            else if (opt == "KEEPTTL") { keep_ttl = true; ++ttl_opts; }
            else if ((opt == "EX" || opt == "PX" || opt == "EXAT" || opt == "PXAT") && i + 1 < argc) {
                long long n;
                if (!to_int64(cmd[++i], n)) { resp::error(out, kNotInt); return true; }
                if (n <= 0) { resp::error(out, kInvalidExpire); return true; }
                if (opt == "EX" && n > LLONG_MAX / 1000) { resp::error(out, kInvalidExpire); return true; }
                if (opt == "EX") expire_at = store_.now() + n * 1000;
                else if (opt == "PX") expire_at = store_.now() + n;
                else if (opt == "EXAT") expire_at = n * 1000;
                else expire_at = n;
                ++ttl_opts;
            } else {
                resp::error(out, kSyntax);
                return true;
            }
        }
        if ((nx && xx) || ttl_opts > 1) { resp::error(out, kSyntax); return true; }

        const std::string* cur = store_.get(cmd[1]);
        std::string old;
        bool had = cur != nullptr;
        if (had) old = *cur;
        bool do_set = !(nx && had) && !(xx && !had);
        if (do_set) {
            store_.set(cmd[1], cmd[2], expire_at, keep_ttl);
            // Log with an absolute deadline so replay is independent of time.
            if (keep_ttl) log({"SET", cmd[1], cmd[2], "KEEPTTL"});
            else if (expire_at != Store::kNoExpiry)
                log({"SET", cmd[1], cmd[2], "PXAT", std::to_string(expire_at)});
            else log({"SET", cmd[1], cmd[2]});
        }
        if (get) had ? resp::bulk_string(out, old) : resp::null_bulk(out);
        else if (do_set) resp::simple_string(out, "OK");
        else resp::null_bulk(out);
        return true;
    }
    if (name == "SETNX") {
        if (argc != 3) { wrong_args(out, name); return true; }
        if (store_.exists(cmd[1])) { resp::integer(out, 0); return true; }
        store_.set(cmd[1], cmd[2]);
        log({"SET", cmd[1], cmd[2]});
        resp::integer(out, 1);
        return true;
    }
    if (name == "MGET") {
        if (argc < 2) { wrong_args(out, name); return true; }
        resp::array_header(out, argc - 1);
        for (std::size_t i = 1; i < argc; ++i) {
            const std::string* v = store_.get(cmd[i]);
            v ? resp::bulk_string(out, *v) : resp::null_bulk(out);
        }
        return true;
    }
    if (name == "MSET") {
        if (argc < 3 || argc % 2 == 0) { wrong_args(out, name); return true; }
        for (std::size_t i = 1; i < argc; i += 2) store_.set(cmd[i], cmd[i + 1]);
        log(cmd);
        resp::simple_string(out, "OK");
        return true;
    }
    if (name == "INCR" || name == "DECR" || name == "INCRBY" || name == "DECRBY") {
        bool by = name.size() == 6;
        if (argc != (by ? 3u : 2u)) { wrong_args(out, name); return true; }
        long long delta = 1;
        if (by && !to_int64(cmd[2], delta)) { resp::error(out, kNotInt); return true; }
        if (name[0] == 'D') {
            if (delta == LLONG_MIN) { resp::error(out, "ERR decrement would overflow"); return true; }
            delta = -delta;
        }
        long long cur = 0;
        if (const std::string* v = store_.get(cmd[1]); v && !to_int64(*v, cur)) {
            resp::error(out, kNotInt);
            return true;
        }
        if ((delta > 0 && cur > LLONG_MAX - delta) || (delta < 0 && cur < LLONG_MIN - delta)) {
            resp::error(out, "ERR increment or decrement would overflow");
            return true;
        }
        cur += delta;
        store_.set(cmd[1], std::to_string(cur), Store::kNoExpiry, /*keep_ttl=*/true);
        log({"SET", cmd[1], std::to_string(cur), "KEEPTTL"});
        resp::integer(out, cur);
        return true;
    }
    if (name == "APPEND") {
        if (argc != 3) { wrong_args(out, name); return true; }
        std::string* v = store_.get_mut(cmd[1]);
        if (v) *v += cmd[2];
        else store_.set(cmd[1], cmd[2]);
        log(cmd);
        resp::integer(out, static_cast<long long>(v ? v->size() : cmd[2].size()));
        return true;
    }
    if (name == "STRLEN") {
        if (argc != 2) { wrong_args(out, name); return true; }
        const std::string* v = store_.get(cmd[1]);
        resp::integer(out, v ? static_cast<long long>(v->size()) : 0);
        return true;
    }

    // ---- keys ---------------------------------------------------------------
    if (name == "DEL" || name == "UNLINK") {
        if (argc < 2) { wrong_args(out, name); return true; }
        long long n = 0;
        resp::Command logged{"DEL"};
        for (std::size_t i = 1; i < argc; ++i)
            if (store_.del(cmd[i])) { ++n; logged.push_back(cmd[i]); }
        if (n) log(logged);
        resp::integer(out, n);
        return true;
    }
    if (name == "EXISTS") {
        if (argc < 2) { wrong_args(out, name); return true; }
        long long n = 0;
        for (std::size_t i = 1; i < argc; ++i) n += store_.exists(cmd[i]);
        resp::integer(out, n);
        return true;
    }
    if (name == "KEYS") {
        if (argc != 2) { wrong_args(out, name); return true; }
        std::vector<std::string> keys;
        store_.for_each([&](const std::string& k, const std::string&, int64_t) {
            if (glob_match(cmd[1], k)) keys.push_back(k);
        });
        std::sort(keys.begin(), keys.end());
        resp::array_header(out, keys.size());
        for (const auto& k : keys) resp::bulk_string(out, k);
        return true;
    }
    if (name == "EXPIRE" || name == "PEXPIRE" || name == "EXPIREAT" || name == "PEXPIREAT") {
        if (argc != 3) { wrong_args(out, name); return true; }
        long long n;
        if (!to_int64(cmd[2], n)) { resp::error(out, kNotInt); return true; }
        if ((name == "EXPIRE" || name == "EXPIREAT") && (n > LLONG_MAX / 1000 || n < LLONG_MIN / 1000)) {
            resp::error(out, "ERR invalid expire time");
            return true;
        }
        int64_t at = name == "EXPIRE"   ? store_.now() + n * 1000
                   : name == "PEXPIRE"  ? store_.now() + n
                   : name == "EXPIREAT" ? n * 1000
                                        : n;
        bool ok = store_.expire_at(cmd[1], at);
        if (ok) log({"PEXPIREAT", cmd[1], std::to_string(at)});
        resp::integer(out, ok ? 1 : 0);
        return true;
    }
    if (name == "TTL" || name == "PTTL") {
        if (argc != 2) { wrong_args(out, name); return true; }
        int64_t ms = store_.pttl(cmd[1]);
        if (ms < 0 || name == "PTTL") resp::integer(out, ms);
        else resp::integer(out, (ms + 500) / 1000);  // Redis rounds to nearest second
        return true;
    }
    if (name == "PERSIST") {
        if (argc != 2) { wrong_args(out, name); return true; }
        bool ok = store_.persist(cmd[1]);
        if (ok) log(cmd);
        resp::integer(out, ok ? 1 : 0);
        return true;
    }

    resp::error(out, "ERR unknown command '" + cmd[0] + "'");
    return true;
}

}  // namespace kv
