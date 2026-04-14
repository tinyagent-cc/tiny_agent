#pragma once
#include "agent.hpp"
#include <chrono>
#include <iterator>
#include <thread>

namespace tiny_agent::batch {

// ── Result for a single item in the batch ───────────────────────────────────

struct ItemResult {
    std::size_t index;
    std::string input;
    std::optional<std::string> output;
    std::optional<std::string> error;
    int attempts = 1;

    bool ok() const { return output.has_value(); }
    explicit operator bool() const { return ok(); }
};

// ── Lifecycle hooks ─────────────────────────────────────────────────────────
//
//  interceptor  — transform input before processing; return nullopt to skip.
//  on_success   — called after each successful run.
//  on_error     — called on each failed attempt; return true to retry
//                 (only retries if attempts remain per Config::max_retries).
//  on_failure   — called once an item permanently fails.

struct Hooks {
    std::function<std::optional<std::string>(std::size_t index, const std::string& input)>
        interceptor;

    std::function<void(std::size_t index, const std::string& input, const std::string& output)>
        on_success;

    std::function<bool(std::size_t index, const std::string& input,
                       const std::exception& ex, int attempt)>
        on_error;

    std::function<void(std::size_t index, const std::string& input, const std::string& error)>
        on_failure;
};

// ── Batch configuration ─────────────────────────────────────────────────────

struct Config {
    int max_retries = 0;
    std::chrono::milliseconds retry_delay{500};
    bool stop_on_failure = false;
};

// ── Internal: process one item with full hook/retry lifecycle ────────────────

namespace detail {

template<llm_like LLMType>
ItemResult process_one(Agent<LLMType>& agent, std::size_t index,
                       const std::string& raw_input,
                       const Config& cfg, const Hooks& hooks, const Log& log) {
    std::string input = raw_input;

    if (hooks.interceptor) {
        auto transformed = hooks.interceptor(index, raw_input);
        if (!transformed) {
            log.info("batch", "[" + std::to_string(index) + "] skipped by interceptor");
            return {index, raw_input, std::nullopt, "skipped", 0};
        }
        input = std::move(*transformed);
    }

    for (int attempt = 1; attempt <= cfg.max_retries + 1; ++attempt) {
        try {
            auto output = agent.run(input);
            if (hooks.on_success)
                hooks.on_success(index, input, output);
            return {index, raw_input, std::move(output), std::nullopt, attempt};
        } catch (const std::exception& ex) {
            bool can_retry = (attempt <= cfg.max_retries);
            bool should_retry = can_retry;

            if (hooks.on_error)
                should_retry = hooks.on_error(index, input, ex, attempt) && can_retry;

            if (should_retry) {
                auto delay = cfg.retry_delay * (1 << (attempt - 1));
                log.info("batch", "[" + std::to_string(index) + "] retrying in "
                    + std::to_string(delay.count()) + "ms");
                std::this_thread::sleep_for(delay);
                continue;
            }

            std::string err = ex.what();
            if (hooks.on_failure)
                hooks.on_failure(index, input, err);
            return {index, raw_input, std::nullopt, std::move(err), attempt};
        }
    }

    return {index, raw_input, std::nullopt, "max retries exhausted", cfg.max_retries + 1};
}

} // namespace detail

// ── Eager batch — process all inputs, return collected results ───────────────

template<llm_like LLMType>
std::vector<ItemResult> run(Agent<LLMType>& agent,
                            const std::vector<std::string>& inputs,
                            Config config = {}, Hooks hooks = {},
                            Log log = {}) {
    log.info("batch", "processing " + std::to_string(inputs.size()) + " items"
        + " (max_retries=" + std::to_string(config.max_retries)
        + " stop_on_failure=" + std::to_string(config.stop_on_failure) + ")");

    std::vector<ItemResult> results;
    results.reserve(inputs.size());

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto result = detail::process_one(agent, i, inputs[i], config, hooks, log);
        bool failed = !result.ok();
        results.push_back(std::move(result));
        if (failed && config.stop_on_failure) {
            log.warn("batch", "stopping at index " + std::to_string(i));
            break;
        }
    }

    auto ok = std::count_if(results.begin(), results.end(),
                            [](auto& r) { return r.ok(); });
    log.info("batch", std::to_string(ok) + "/" + std::to_string(results.size()) + " succeeded");
    return results;
}

// ── Lazy iterator — process items one at a time on demand ───────────────────

template<llm_like LLMType>
class Iterator {
    Agent<LLMType>* agent_;
    std::vector<std::string> inputs_;
    Config  config_;
    Hooks   hooks_;
    Log     log_;
    std::size_t pos_ = 0;

public:
    Iterator(Agent<LLMType>& agent, std::vector<std::string> inputs,
             Config config = {}, Hooks hooks = {}, Log log = {})
        : agent_(&agent), inputs_(std::move(inputs))
        , config_(std::move(config)), hooks_(std::move(hooks))
        , log_(std::move(log)) {}

    bool has_next() const { return pos_ < inputs_.size(); }
    std::size_t remaining() const { return inputs_.size() - pos_; }
    std::size_t size() const { return inputs_.size(); }
    std::size_t position() const { return pos_; }

    ItemResult next() {
        if (!has_next()) throw Error("batch iterator exhausted");
        auto result = detail::process_one(*agent_, pos_, inputs_[pos_],
                                          config_, hooks_, log_);
        ++pos_;
        return result;
    }

    std::vector<ItemResult> collect() {
        std::vector<ItemResult> results;
        results.reserve(remaining());
        while (has_next()) {
            auto r = next();
            bool failed = !r.ok();
            results.push_back(std::move(r));
            if (failed && config_.stop_on_failure) break;
        }
        return results;
    }

    // ── Range-for support ───────────────────────────────────────────────

    class Cursor {
        Iterator* parent_ = nullptr;
        std::optional<ItemResult> current_;

        void fetch() {
            if (parent_ && parent_->has_next())
                current_ = parent_->next();
            else
                current_.reset();
        }

    public:
        Cursor() = default;
        explicit Cursor(Iterator& parent) : parent_(&parent) { fetch(); }

        const ItemResult& operator*()  const { return *current_; }
        const ItemResult* operator->() const { return &*current_; }
        Cursor& operator++() { fetch(); return *this; }

        friend bool operator==(const Cursor& c, std::default_sentinel_t) {
            return !c.current_.has_value();
        }
        friend bool operator!=(const Cursor& c, std::default_sentinel_t s) {
            return !(c == s);
        }
    };

    Cursor begin() { return Cursor{*this}; }
    std::default_sentinel_t end() const { return {}; }
};

// ── Factory ─────────────────────────────────────────────────────────────────

template<llm_like LLMType>
Iterator<LLMType> iterate(Agent<LLMType>& agent,
                          std::vector<std::string> inputs,
                          Config config = {}, Hooks hooks = {},
                          Log log = {}) {
    return {agent, std::move(inputs), std::move(config),
            std::move(hooks), std::move(log)};
}

// ── Summary helper ──────────────────────────────────────────────────────────

struct Summary {
    std::size_t total     = 0;
    std::size_t succeeded = 0;
    std::size_t failed    = 0;
    std::size_t skipped   = 0;
};

inline Summary summarize(const std::vector<ItemResult>& results) {
    Summary s{.total = results.size()};
    for (auto& r : results) {
        if (r.ok())           ++s.succeeded;
        else if (r.attempts == 0) ++s.skipped;
        else                      ++s.failed;
    }
    return s;
}

} // namespace tiny_agent::batch
