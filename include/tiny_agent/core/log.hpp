#pragma once
#include <string_view>
#include <ostream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <functional>

namespace tiny_agent {

enum class LogLevel { trace, debug, info, warn, error, off };

constexpr const char* to_string(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::trace: return "TRACE";
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info:  return "INFO";
        case LogLevel::warn:  return "WARN";
        case LogLevel::error: return "ERROR";
        case LogLevel::off:   return "OFF";
    }
    return "UNKNOWN";
}

class Log {
    std::reference_wrapper<std::ostream> os_ = std::ref(std::cerr);
    LogLevel level_      = LogLevel::warn;
    bool     timestamps_ = false;

public:
    Log() = default;
    Log(LogLevel level) : level_(level) {}
    Log(std::ostream& os, LogLevel level = LogLevel::warn)
        : os_(os), level_(level) {}

    void set_level(LogLevel level)      { level_ = level; }
    void set_timestamps(bool enable)    { timestamps_ = enable; }
    [[nodiscard]] LogLevel level() const { return level_; }

    void log(LogLevel lvl, std::string_view tag, std::string_view msg) const {
        if (lvl < level_) return;
        auto& os = os_.get();
        if (timestamps_) write_timestamp(os);
        os << "[" << to_string(lvl) << "] [" << tag << "] " << msg << "\n";
    }

    void trace(std::string_view tag, std::string_view msg) const { log(LogLevel::trace, tag, msg); }
    void debug(std::string_view tag, std::string_view msg) const { log(LogLevel::debug, tag, msg); }
    void info (std::string_view tag, std::string_view msg) const { log(LogLevel::info,  tag, msg); }
    void warn (std::string_view tag, std::string_view msg) const { log(LogLevel::warn,  tag, msg); }
    void error(std::string_view tag, std::string_view msg) const { log(LogLevel::error, tag, msg); }

private:
    static void write_timestamp(std::ostream& os) {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() % 1000;
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[20];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        os << buf << "." << std::setfill('0') << std::setw(3) << ms << " ";
    }
};

} // namespace tiny_agent
