// Command dispatch: executes one parsed command against the store and
// appends the RESP reply to `out`. Commands that change the keyspace are
// logged to the AOF in a deterministic form (relative TTLs become absolute).
#pragma once

#include <string>

#include "aof.h"
#include "resp.h"
#include "store.h"

namespace kv {

struct ServerStats {
    std::size_t connected_clients = 0;
    std::size_t total_connections = 0;
    std::size_t total_commands = 0;
    std::size_t expired_keys = 0;
};

class CommandExecutor {
public:
    CommandExecutor(Store& store, Aof* aof, ServerStats* stats = nullptr)
        : store_(store), aof_(aof), stats_(stats) {}

    // Returns false if the connection should be closed after the reply (QUIT).
    bool execute(const resp::Command& cmd, std::string& out);

    // Applies a command read back from the AOF (no reply, no re-logging).
    void replay(const resp::Command& cmd);

private:
    void log(const resp::Command& cmd);

    Store& store_;
    Aof* aof_;
    ServerStats* stats_;
    bool replaying_ = false;
};

// Redis-style glob matching (*, ?, [abc], [a-z], [^x], \x) used by KEYS.
bool glob_match(std::string_view pattern, std::string_view str);

}  // namespace kv
