// RESP (REdis Serialization Protocol) parsing and encoding.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kv::resp {

using Command = std::vector<std::string>;

enum class ParseStatus { Ok, Incomplete, Error };

struct ParseResult {
    ParseStatus status;
    Command command;       // valid when status == Ok
    std::size_t consumed;  // bytes consumed from the input when status == Ok
    std::string error;     // set when status == Error
};

// Parse one command from the front of `buf`. Accepts both RESP arrays of
// bulk strings (what clients send) and inline commands ("PING\r\n").
// Returns Incomplete if more bytes are needed; never consumes partial input.
ParseResult parse_command(std::string_view buf);

// Encoders append to `out` so a whole pipeline of replies can be batched.
void simple_string(std::string& out, std::string_view s);
void error(std::string& out, std::string_view msg);
void integer(std::string& out, long long n);
void bulk_string(std::string& out, std::string_view s);
void null_bulk(std::string& out);
void array_header(std::string& out, std::size_t n);

// Encode a command as a RESP array (used for the append-only file).
std::string encode_command(const Command& cmd);

}  // namespace kv::resp
