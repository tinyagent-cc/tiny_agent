#pragma once
#include "../core/middleware.hpp"
#include "../integrations/rete_convert.hpp"
#include <rete/rete.hpp>
#include <array>
#include <map>
#include <tuple>

namespace tiny_agent::middleware {

using Fact = std::array<rete::Value, 3>;

class ReflexOutcome {
    std::optional<std::string> text_;
    std::vector<ToolCall> calls_;
    std::map<int, std::string> vetoes_;
    std::vector<std::tuple<int, std::string, json>> arg_replacements_;
    int next_id_ = 0;
public:
    void respond(std::string text) { text_ = std::move(text); }

    void call_tool(std::string name, json args) {
        calls_.push_back({"reflex-" + std::to_string(next_id_++),
                          std::move(name), std::move(args)});
    }

    void veto(int tool_call_index, std::string reason) {
        vetoes_[tool_call_index] = std::move(reason);
    }

    void replace_arg(int tool_call_index, std::string key, json value) {
        arg_replacements_.emplace_back(tool_call_index, std::move(key), std::move(value));
    }

    bool triggered() const { return text_.has_value() || !calls_.empty(); }
    bool has_guardrails() const { return !vetoes_.empty() || !arg_replacements_.empty(); }

    void reset() {
        text_.reset(); calls_.clear(); vetoes_.clear();
        arg_replacements_.clear(); next_id_ = 0;
    }

    LLMResponse to_response() const {
        Message m = Message::assistant(text_.value_or(""));
        m.tool_calls = calls_;
        return {std::move(m), json{{"reflex", true}}, "reflex", json{{"reflex", true}}};
    }

    void apply_guardrails(LLMResponse& resp) const {
        auto& calls = resp.message.tool_calls;
        for (auto& [idx, key, value] : arg_replacements_)
            if (idx >= 0 && idx < static_cast<int>(calls.size()))
                calls[static_cast<size_t>(idx)].arguments[key] = value;

        if (!vetoes_.empty()) {
            json veto_log = json::array();
            // erase back-to-front so indices stay valid
            for (auto it = vetoes_.rbegin(); it != vetoes_.rend(); ++it) {
                if (it->first >= 0 && it->first < static_cast<int>(calls.size())) {
                    veto_log.push_back({{"tool", calls[static_cast<size_t>(it->first)].name},
                                        {"reason", it->second}});
                    calls.erase(calls.begin() + it->first);
                }
            }
            resp.raw["reflex_vetoes"] = veto_log;
            // a response that was only vetoed calls must not read as a silent
            // empty answer to the agent loop
            if (calls.empty() && resp.message.text().empty() && !veto_log.empty()) {
                std::string note = "Blocked by reflex guardrail:";
                for (auto& v : veto_log)
                    note += " " + v["tool"].get<std::string>() +
                            " (" + v["reason"].get<std::string>() + ");";
                resp.message.content = note;
                resp.finish_reason = "reflex_guardrail";
            }
        }
    }
};

struct ReflexConfig {
    std::function<std::vector<Fact>(const std::vector<Message>&)> extract_facts;
    std::function<std::vector<Fact>(const LLMResponse&)> extract_response_facts;
    int max_cycles = 256;
};

// Owns the rete network and the per-run outcome. Register rules on engine();
// rule actions capture outcome() by reference. The ReflexEngine must outlive
// every chain its middleware() was added to. Not thread-safe: one concurrent
// run at a time.
class ReflexEngine {
    rete::ReteEngine engine_;
    ReflexOutcome outcome_;
    std::vector<rete::WmePtr> transient_;

    void assert_transient(const std::vector<Fact>& facts) {
        for (auto& f : facts)
            transient_.push_back(engine_.assert_fact(f[0], f[1], f[2]));
    }
    void retract_transient() {
        for (auto& w : transient_) engine_.retract_fact(w);
        transient_.clear();
    }

    // Retracts transient WMEs on scope exit no matter how the scope is left —
    // a rule action that throws during engine_.run() must not leave transients
    // asserted forever, or later runs get spurious matches against stale facts.
    struct TransientGuard {
        ReflexEngine* self;
        ~TransientGuard() { self->retract_transient(); }
    };

public:
    rete::ReteEngine& engine() { return engine_; }
    ReflexOutcome& outcome() { return outcome_; }

    MiddlewareFn middleware(ReflexConfig cfg) {
        return [this, cfg = std::move(cfg)](std::vector<Message>& msgs, Next next) -> LLMResponse {
            // Facts get fresh WmeIds every assert, so refraction never blocks a
            // later identical-looking event on WmeId alone; clearing here just
            // bounds refraction_set_'s growth over a long-lived engine.
            engine_.clear_refraction();
            if (cfg.extract_facts) {
                outcome_.reset();
                assert_transient(cfg.extract_facts(msgs));
                {
                    TransientGuard guard{this};
                    engine_.run(cfg.max_cycles);
                }
                if (outcome_.triggered()) return outcome_.to_response();
            }
            auto resp = next(msgs);
            if (cfg.extract_response_facts && resp.message.has_tool_calls()) {
                outcome_.reset();
                assert_transient(cfg.extract_response_facts(resp));
                {
                    TransientGuard guard{this};
                    engine_.run(cfg.max_cycles);
                }
                outcome_.apply_guardrails(resp);
            }
            return resp;
        };
    }
};

} // namespace tiny_agent::middleware
