#pragma once
#include "../core/types.hpp"
#include <concepts>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <cstring>
#endif

namespace tiny_agent::mcp {

// ── Concept for transports ──────────────────────────────────────────────────

template<typename T>
concept transport_like = requires(T t, const json& req) {
    { t.send(req) } -> std::same_as<void>;
    { t.receive() } -> std::same_as<json>;
};

#ifndef _WIN32

class StdioTransport {
    pid_t pid_     = -1;
    int   to_fd_   = -1;
    int   from_fd_ = -1;

public:
    StdioTransport(const std::string& command, const std::vector<std::string>& args) {
        int to_child[2], from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0)
            throw MCPError("pipe() failed");

        pid_ = fork();
        if (pid_ < 0) throw MCPError("fork() failed");

        if (pid_ == 0) {
            close(to_child[1]);
            close(from_child[0]);
            dup2(to_child[0],   STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]);
            close(from_child[1]);

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command.c_str()));
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(command.c_str(), argv.data());
            _exit(127);
        }

        close(to_child[0]);
        close(from_child[1]);
        to_fd_   = to_child[1];
        from_fd_ = from_child[0];
    }

    ~StdioTransport() {
        if (to_fd_   >= 0) close(to_fd_);
        if (from_fd_ >= 0) close(from_fd_);
        if (pid_ > 0) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
    }

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    StdioTransport(StdioTransport&& o) noexcept
        : pid_(o.pid_), to_fd_(o.to_fd_), from_fd_(o.from_fd_)
    { o.pid_ = -1; o.to_fd_ = -1; o.from_fd_ = -1; }

    StdioTransport& operator=(StdioTransport&& o) noexcept {
        std::swap(pid_, o.pid_);
        std::swap(to_fd_, o.to_fd_);
        std::swap(from_fd_, o.from_fd_);
        return *this;
    }

    void send(const json& request) {
        auto data = request.dump() + "\n";
        auto written = ::write(to_fd_, data.c_str(), data.size());
        if (written < 0) throw MCPError("write to MCP process failed");
    }

    json receive() {
        constexpr int max_attempts = 1000;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            std::string line;
            char c;
            while (true) {
                auto n = ::read(from_fd_, &c, 1);
                if (n <= 0) throw MCPError("MCP process closed");
                if (c == '\n') break;
                line += c;
            }
            if (line.empty()) continue;
            try { return json::parse(line); }
            catch (...) { continue; }
        }
        throw MCPError("MCP transport: no valid JSON after "
            + std::to_string(max_attempts) + " lines");
    }
};

static_assert(transport_like<StdioTransport>);

#endif // _WIN32

} // namespace tiny_agent::mcp
