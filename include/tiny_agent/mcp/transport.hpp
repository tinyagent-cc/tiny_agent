#pragma once
#include "../core/types.hpp"
#include <concepts>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <cstring>
#include <algorithm>
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
    std::string read_buf_;
    static constexpr std::size_t buf_size_ = 8192;

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
        if (this == &o) return *this;
        if (to_fd_ >= 0) close(to_fd_);
        if (from_fd_ >= 0) close(from_fd_);
        if (pid_ > 0) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
        pid_ = o.pid_; o.pid_ = -1;
        to_fd_ = o.to_fd_; o.to_fd_ = -1;
        from_fd_ = o.from_fd_; o.from_fd_ = -1;
        return *this;
    }

    void send(const json& request) {
        auto data = request.dump() + "\n";
        const char* ptr = data.data();
        std::size_t remaining = data.size();
        while (remaining > 0) {
            auto written = ::write(to_fd_, ptr, remaining);
            if (written < 0) throw MCPError("write to MCP process failed");
            ptr += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }

    json receive() {
        constexpr int max_attempts = 100;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            auto nl = read_buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = read_buf_.substr(0, nl);
                read_buf_.erase(0, nl + 1);
                if (line.empty()) continue;
                try { return json::parse(line); }
                catch (...) { continue; }
            }
            char buf[buf_size_];
            auto n = ::read(from_fd_, buf, sizeof(buf));
            if (n <= 0) throw MCPError("MCP process closed");
            read_buf_.append(buf, static_cast<std::size_t>(n));
        }
        throw MCPError("MCP transport: no valid JSON after "
            + std::to_string(max_attempts) + " read attempts");
    }
};

static_assert(transport_like<StdioTransport>);

#endif // _WIN32

} // namespace tiny_agent::mcp
