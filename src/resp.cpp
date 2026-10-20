#include "resp.h"

#include <charconv>

namespace kv::resp {
namespace {

constexpr std::size_t kMaxBulkLen = 512 * 1024 * 1024;  // same cap as Redis
constexpr long long kMaxArrayLen = 1024 * 1024;
constexpr std::size_t kMaxInlineLen = 64 * 1024;

// Find "\r\n" starting at `pos`; returns npos if not yet present.
std::size_t find_crlf(std::string_view buf, std::size_t pos) {
    auto i = buf.find("\r\n", pos);
    return i;
}

bool parse_int(std::string_view s, long long& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

ParseResult incomplete() { return {ParseStatus::Incomplete, {}, 0, {}}; }
ParseResult fail(std::string msg) { return {ParseStatus::Error, {}, 0, std::move(msg)}; }

ParseResult parse_inline(std::string_view buf) {
    auto end = buf.find('\n');
    if (end == std::string_view::npos) {
        if (buf.size() > kMaxInlineLen) return fail("Protocol error: too big inline request");
        return incomplete();
    }
    std::string_view line = buf.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    Command cmd;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) cmd.emplace_back(line.substr(start, i - start));
    }
    return {ParseStatus::Ok, std::move(cmd), end + 1, {}};
}

}  // namespace

ParseResult parse_command(std::string_view buf) {
    if (buf.empty()) return incomplete();
    if (buf[0] != '*') return parse_inline(buf);

    std::size_t line_end = find_crlf(buf, 1);
    if (line_end == std::string_view::npos) return incomplete();
    long long count = 0;
    if (!parse_int(buf.substr(1, line_end - 1), count) || count > kMaxArrayLen)
        return fail("Protocol error: invalid multibulk length");

    std::size_t pos = line_end + 2;
    Command cmd;
    if (count > 0) cmd.reserve(static_cast<std::size_t>(count));

    for (long long n = 0; n < count; ++n) {
        if (pos >= buf.size()) return incomplete();
        if (buf[pos] != '$')
            return fail(std::string("Protocol error: expected '$', got '") + buf[pos] + "'");
        line_end = find_crlf(buf, pos + 1);
        if (line_end == std::string_view::npos) return incomplete();
        long long len = 0;
        if (!parse_int(buf.substr(pos + 1, line_end - pos - 1), len) || len < 0 ||
            static_cast<std::size_t>(len) > kMaxBulkLen)
            return fail("Protocol error: invalid bulk length");
        pos = line_end + 2;
        auto ulen = static_cast<std::size_t>(len);
        if (buf.size() < pos + ulen + 2) return incomplete();
        if (buf[pos + ulen] != '\r' || buf[pos + ulen + 1] != '\n')
            return fail("Protocol error: bulk string not terminated by CRLF");
        cmd.emplace_back(buf.substr(pos, ulen));
        pos += ulen + 2;
    }
    return {ParseStatus::Ok, std::move(cmd), pos, {}};
}

void simple_string(std::string& out, std::string_view s) {
    out += '+';
    out += s;
    out += "\r\n";
}

void error(std::string& out, std::string_view msg) {
    out += '-';
    out += msg;
    out += "\r\n";
}

void integer(std::string& out, long long n) {
    out += ':';
    out += std::to_string(n);
    out += "\r\n";
}

void bulk_string(std::string& out, std::string_view s) {
    out += '$';
    out += std::to_string(s.size());
    out += "\r\n";
    out += s;
    out += "\r\n";
}

void null_bulk(std::string& out) { out += "$-1\r\n"; }

void array_header(std::string& out, std::size_t n) {
    out += '*';
    out += std::to_string(n);
    out += "\r\n";
}

std::string encode_command(const Command& cmd) {
    std::string out;
    array_header(out, cmd.size());
    for (const auto& arg : cmd) bulk_string(out, arg);
    return out;
}

}  // namespace kv::resp
