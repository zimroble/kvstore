#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "server.h"

namespace {

kv::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--bind ADDR] [--aof FILE] [--fsync always|everysec|no]\n"
                 "          [--hz N]\n",
                 argv0);
}

}  // namespace

int main(int argc, char** argv) {
    kv::ServerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if (a == "--port") cfg.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--bind") cfg.bind = next();
        else if (a == "--aof") cfg.aof_path = next();
        else if (a == "--fsync") {
            if (!kv::parse_fsync_policy(next(), cfg.fsync)) { usage(argv[0]); return 2; }
        } else if (a == "--hz") cfg.expire_hz = std::atoi(next());
        else { usage(argv[0]); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    kv::Server server(cfg);
    if (!server.init()) return 1;
    g_server = &server;

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    std::fprintf(stderr, "kvstore: listening on %s:%u%s\n", cfg.bind.c_str(), server.port(),
                 cfg.aof_path.empty() ? " (persistence off)" : "");
    server.run();
    std::fprintf(stderr, "kvstore: shut down cleanly\n");
    return 0;
}
