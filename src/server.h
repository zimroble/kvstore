// Single-threaded, non-blocking TCP server built on Linux epoll.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "aof.h"
#include "commands.h"
#include "store.h"

namespace kv {

struct ServerConfig {
    std::string bind = "127.0.0.1";
    uint16_t port = 6379;
    std::string aof_path;  // empty = persistence disabled
    FsyncPolicy fsync = FsyncPolicy::EverySec;
    int expire_hz = 10;    // active-expiry cycles per second
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    // Loads the AOF and binds the listening socket. Returns false (and logs)
    // on failure.
    bool init();

    // Runs the event loop until stop() is called (async-signal-safe).
    void run();
    void stop() { stop_.store(true); }

    uint16_t port() const { return bound_port_; }

private:
    struct Conn {
        int fd;
        std::string rbuf;
        std::string wbuf;
        std::size_t wpos = 0;
        bool close_after_write = false;
    };

    void accept_clients();
    void handle_read(Conn& c);
    bool handle_write(Conn& c);  // false = connection closed
    void close_conn(int fd);
    void update_interest(Conn& c);
    void cron(int64_t now);

    ServerConfig cfg_;
    Store store_;
    std::unique_ptr<Aof> aof_;
    ServerStats stats_;
    CommandExecutor exec_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
    std::vector<int> pending_writes_;
    std::atomic<bool> stop_{false};
    int64_t last_cron_ms_ = 0;
};

}  // namespace kv
