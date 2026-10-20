#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace kv {
namespace {

constexpr std::size_t kMaxQueryBuffer = 64 * 1024 * 1024;
constexpr int kMaxEvents = 256;

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void log_err(const char* what) { std::fprintf(stderr, "kvstore: %s: %s\n", what, std::strerror(errno)); }

}  // namespace

Server::Server(ServerConfig cfg)
    : cfg_(std::move(cfg)),
      aof_(cfg_.aof_path.empty() ? nullptr : std::make_unique<Aof>(cfg_.aof_path, cfg_.fsync)),
      exec_(store_, aof_.get(), &stats_) {}

Server::~Server() {
    for (auto& [fd, c] : conns_) ::close(fd);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool Server::init() {
    if (aof_) {
        auto stats = aof_->replay([this](const resp::Command& cmd) { exec_.replay(cmd); });
        if (!stats.ok) {
            std::fprintf(stderr, "kvstore: %s\n", stats.error.c_str());
            return false;
        }
        std::fprintf(stderr, "kvstore: replayed %zu commands from %s (%zu keys)\n", stats.commands,
                     aof_->path().c_str(), store_.size());
        if (stats.truncated_bytes)
            std::fprintf(stderr, "kvstore: dropped %zu-byte torn write at end of AOF\n",
                         stats.truncated_bytes);
        if (!aof_->open()) { log_err("open AOF"); return false; }
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) { log_err("socket"); return false; }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.bind.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "kvstore: invalid bind address %s\n", cfg_.bind.c_str());
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { log_err("bind"); return false; }
    if (::listen(listen_fd_, 511) < 0) { log_err("listen"); return false; }
    set_nonblocking(listen_fd_);

    socklen_t len = sizeof addr;
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);

    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) { log_err("epoll_create1"); return false; }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
    return true;
}

void Server::run() {
    epoll_event events[kMaxEvents];
    const int cron_interval = 1000 / std::max(1, cfg_.expire_hz);

    while (!stop_.load()) {
        int n = epoll_wait(epoll_fd_, events, kMaxEvents, cron_interval);
        if (n < 0 && errno != EINTR) { log_err("epoll_wait"); break; }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) { accept_clients(); continue; }
            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            Conn& c = *it->second;
            if (events[i].events & (EPOLLERR | EPOLLHUP) && !(events[i].events & EPOLLIN)) {
                close_conn(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) handle_read(c);
            if (events[i].events & EPOLLOUT) pending_writes_.push_back(fd);
        }

        const int64_t now = system_now_ms();
        // Persist this batch of writes *before* acknowledging any of them.
        if (aof_ && !aof_->flush(now)) {
            log_err("AOF write failed; shutting down to avoid acknowledging lost writes");
            break;
        }
        for (int fd : pending_writes_) {
            auto it = conns_.find(fd);
            if (it != conns_.end()) handle_write(*it->second);
        }
        pending_writes_.clear();

        if (now - last_cron_ms_ >= cron_interval) {
            cron(now);
            last_cron_ms_ = now;
        }
    }
    if (aof_) aof_->flush(system_now_ms());
}

void Server::cron(int64_t /*now*/) { stats_.expired_keys += store_.active_expire_cycle(); }

void Server::accept_clients() {
    for (;;) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) log_err("accept");
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) { ::close(fd); continue; }
        conns_.emplace(fd, std::make_unique<Conn>(Conn{fd, {}, {}, 0, false}));
        ++stats_.connected_clients;
        ++stats_.total_connections;
    }
}

void Server::handle_read(Conn& c) {
    char buf[16 * 1024];
    bool eof = false;
    for (;;) {
        ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n > 0) {
            c.rbuf.append(buf, static_cast<std::size_t>(n));
            if (c.rbuf.size() > kMaxQueryBuffer) { eof = true; break; }
            continue;
        }
        if (n == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
        break;
    }

    // Execute every complete command in the buffer (pipelining).
    std::size_t off = 0;
    while (!c.close_after_write && off < c.rbuf.size()) {
        auto res = resp::parse_command(std::string_view(c.rbuf).substr(off));
        if (res.status == resp::ParseStatus::Incomplete) break;
        if (res.status == resp::ParseStatus::Error) {
            resp::error(c.wbuf, res.error);
            c.close_after_write = true;
            break;
        }
        off += res.consumed;
        if (!exec_.execute(res.command, c.wbuf)) c.close_after_write = true;
    }
    c.rbuf.erase(0, off);

    if (eof && c.wbuf.size() == c.wpos) {
        close_conn(c.fd);
        return;
    }
    if (eof) c.close_after_write = true;
    if (c.wbuf.size() > c.wpos) pending_writes_.push_back(c.fd);
}

bool Server::handle_write(Conn& c) {
    while (c.wpos < c.wbuf.size()) {
        ssize_t n = ::send(c.fd, c.wbuf.data() + c.wpos, c.wbuf.size() - c.wpos, MSG_NOSIGNAL);
        if (n > 0) { c.wpos += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close_conn(c.fd);
        return false;
    }
    if (c.wpos == c.wbuf.size()) {
        c.wbuf.clear();
        c.wpos = 0;
        if (c.close_after_write) { close_conn(c.fd); return false; }
    }
    update_interest(c);
    return true;
}

void Server::update_interest(Conn& c) {
    epoll_event ev{};
    ev.events = EPOLLIN | (c.wpos < c.wbuf.size() ? EPOLLOUT : 0u);
    ev.data.fd = c.fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev);
}

void Server::close_conn(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns_.erase(fd);
    --stats_.connected_clients;
}

}  // namespace kv
