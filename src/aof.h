// Append-only file persistence.
//
// Every write command is encoded as RESP and appended to the log. Writes are
// buffered for the duration of one event-loop iteration and flushed (and
// optionally fsync'd) *before* replies are sent, so with FsyncPolicy::Always a
// client never sees "+OK" for a write that is not on disk.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "resp.h"
#include "store.h"

namespace kv {

enum class FsyncPolicy { Always, EverySec, No };

bool parse_fsync_policy(const std::string& s, FsyncPolicy& out);

struct ReplayStats {
    std::size_t commands = 0;
    std::size_t truncated_bytes = 0;  // incomplete tail dropped after a crash
    bool ok = true;
    std::string error;
};

class Aof {
public:
    Aof(std::string path, FsyncPolicy policy);
    ~Aof();
    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // Replays the file into `apply` (missing file = empty log). An incomplete
    // trailing command (torn write) is truncated away; a corrupt command in
    // the middle of the file is a hard error.
    ReplayStats replay(const std::function<void(const resp::Command&)>& apply);

    // Opens the file for appending. Call after replay().
    bool open();

    void append(const resp::Command& cmd);

    // Writes buffered commands to the file; fsyncs according to the policy.
    // Returns false on I/O error.
    bool flush(int64_t now_ms);

    // Compacts the log: writes a minimal set of commands that recreates the
    // current keyspace to a temp file, fsyncs it, and atomically renames it
    // over the old log.
    bool rewrite(Store& store);

    const std::string& path() const { return path_; }
    std::size_t pending_bytes() const { return buf_.size(); }

private:
    std::string path_;
    FsyncPolicy policy_;
    int fd_ = -1;
    std::string buf_;
    int64_t last_fsync_ms_ = 0;
    bool dirty_ = false;  // written but not fsync'd (EverySec)
};

}  // namespace kv
