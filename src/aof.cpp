#include "aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kv {
namespace {

bool write_all(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

bool parse_fsync_policy(const std::string& s, FsyncPolicy& out) {
    if (s == "always") out = FsyncPolicy::Always;
    else if (s == "everysec") out = FsyncPolicy::EverySec;
    else if (s == "no") out = FsyncPolicy::No;
    else return false;
    return true;
}

Aof::Aof(std::string path, FsyncPolicy policy) : path_(std::move(path)), policy_(policy) {}

Aof::~Aof() {
    if (fd_ >= 0) {
        flush(0);
        ::fsync(fd_);
        ::close(fd_);
    }
}

ReplayStats Aof::replay(const std::function<void(const resp::Command&)>& apply) {
    ReplayStats stats;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return stats;  // no log yet
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();

    std::string_view view(data);
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto res = resp::parse_command(view.substr(offset));
        if (res.status == resp::ParseStatus::Incomplete) {
            stats.truncated_bytes = data.size() - offset;
            break;
        }
        if (res.status == resp::ParseStatus::Error) {
            stats.ok = false;
            stats.error = "corrupt AOF at byte " + std::to_string(offset) + ": " + res.error;
            return stats;
        }
        apply(res.command);
        ++stats.commands;
        offset += res.consumed;
    }
    if (stats.truncated_bytes > 0 && ::truncate(path_.c_str(), static_cast<off_t>(offset)) != 0) {
        stats.ok = false;
        stats.error = std::string("failed to truncate torn AOF tail: ") + std::strerror(errno);
    }
    return stats;
}

bool Aof::open() {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    return fd_ >= 0;
}

void Aof::append(const resp::Command& cmd) { buf_ += resp::encode_command(cmd); }

bool Aof::flush(int64_t now_ms) {
    if (fd_ < 0) return true;
    if (!buf_.empty()) {
        if (!write_all(fd_, buf_.data(), buf_.size())) return false;
        buf_.clear();
        dirty_ = true;
    }
    if (!dirty_) return true;
    if (policy_ == FsyncPolicy::Always ||
        (policy_ == FsyncPolicy::EverySec && now_ms - last_fsync_ms_ >= 1000)) {
        if (::fdatasync(fd_) != 0) return false;
        dirty_ = false;
        last_fsync_ms_ = now_ms;
    }
    return true;
}

bool Aof::rewrite(Store& store) {
    if (!flush(store.now())) return false;
    const std::string tmp = path_ + ".rewrite.tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    std::string out;
    bool ok = true;
    store.for_each([&](const std::string& key, const std::string& value, int64_t expire_at) {
        if (expire_at == Store::kNoExpiry)
            out += resp::encode_command({"SET", key, value});
        else
            out += resp::encode_command({"SET", key, value, "PXAT", std::to_string(expire_at)});
        if (out.size() >= (1 << 20)) {
            ok = ok && write_all(fd, out.data(), out.size());
            out.clear();
        }
    });
    ok = ok && write_all(fd, out.data(), out.size());
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path_.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    // fsync the directory so the rename itself survives a crash.
    std::string dir = path_.substr(0, path_.find_last_of('/') + 1);
    int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    // Re-open so further appends go to the new file.
    if (fd_ >= 0) ::close(fd_);
    dirty_ = false;
    return open();
}

}  // namespace kv
