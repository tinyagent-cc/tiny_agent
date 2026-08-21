// 20_context_management — keeping a long conversation inside a token budget
//
// A tool-calling agent fills its context window fast: every turn adds an
// assistant message, a tool call and a tool result, and tool results are the
// bulk of it. context_management() states a budget and, when the conversation
// crosses it, escalates through three techniques in cost order until it fits.
//
// This example needs no API key or network — it builds a synthetic conversation
// and prints what each stage does to it.
//
//   ./build/examples/20_context_management

#include <tiny_agent/middleware/context.hpp>
#include <iomanip>
#include <iostream>

using namespace tiny_agent;
using namespace tiny_agent::middleware;

// A conversation the way an agent loop actually produces one: a system prompt, a
// question, then repeated (assistant asks for a tool, tool answers at length)
// pairs.
static std::vector<Message> build_conversation(int turns) {
    std::vector<Message> msgs;
    msgs.push_back(Message::system(
        "You are a research assistant. Use the search tool to answer questions."));
    msgs.push_back(Message::user("Summarize what is known about Raspberry Pi 5 thermals."));

    for (int i = 0; i < turns; ++i) {
        auto id = "call_" + std::to_string(i);
        Message call = Message::assistant("");
        call.tool_calls.push_back({id, "search",
            json{{"query", "pi 5 thermal throttling benchmark " + std::to_string(i)}}});
        msgs.push_back(std::move(call));

        auto result = Message::tool_result(id,
            "Result " + std::to_string(i) + ": " + std::string(1200, 'x'));
        result.name = "search";
        msgs.push_back(std::move(result));
    }

    msgs.push_back(Message::user("Now write the summary."));
    return msgs;
}

static void report(const char* label, const std::vector<Message>& msgs) {
    std::size_t tool_bytes = 0, cleared = 0, summaries = 0;
    for (const auto& m : msgs) {
        if (m.role == Role::tool) {
            tool_bytes += m.text().size();
            if (m.text().find("cleared") != std::string::npos) ++cleared;
        }
        if (context::is_summary(m)) ++summaries;
    }
    std::cout << std::left << std::setw(26) << label
              << " messages=" << std::setw(4) << msgs.size()
              << " tokens=" << std::setw(7) << approx_token_count(msgs)
              << " tool bytes=" << std::setw(8) << tool_bytes
              << " cleared=" << cleared
              << " summaries=" << summaries << "\n";
}

int main() {
    auto original = build_conversation(12);
    std::cout << "A 12-turn tool-calling conversation:\n\n";
    report("original", original);

    // Each stage on its own, to show what it costs and what it buys.
    std::cout << "\nEach technique applied alone:\n\n";
    {
        auto msgs = original;
        context::clear_tool_results(msgs, 3, "[tool output cleared]");
        report("1. clear tool output", msgs);
    }
    {
        auto msgs = original;
        context::summarize_middle(msgs, 4,
            [](const std::vector<Message>& m) { return extractive_summarize(m); },
            "[Conversation summary]\n");
        report("2. summarize the middle", msgs);
    }
    {
        auto msgs = original;
        context::trim_oldest(msgs, {.max_tokens = 2000, .target_ratio = 0.5, .keep_recent = 4});
        report("3. drop the oldest", msgs);
    }

    // The ladder: escalate only as far as the budget requires.
    std::cout << "\ncontext_management() at four budgets, escalating as needed:\n\n";
    for (std::size_t max_tokens : {8000u, 3000u, 1200u, 400u}) {
        auto msgs = original;
        auto mw = context_management({
            .budget = {.max_tokens = max_tokens, .target_ratio = 0.6, .keep_recent = 4},
            .keep_tool_results = 2,
            .log = Log{std::cout, LogLevel::info}});

        // The middleware hands the compacted messages to whatever comes next;
        // here that is a stand-in for the model call.
        mw(msgs, [](std::vector<Message>&) {
            return LLMResponse{Message::assistant("(model called)"), {}, "stop", {}};
        });
        report(("budget " + std::to_string(max_tokens)).c_str(), msgs);
        std::cout << "\n";
    }

    // Wiring it into an agent is one line.
    std::cout << "In an agent:\n\n"
              << "    AgentConfig cfg;\n"
              << "    cfg.middlewares.push_back(middleware::context_management({\n"
              << "        .budget = {.max_tokens = 8000, .keep_recent = 6}}));\n";

    // Whatever the pressure, the conversation stays valid: no tool result is
    // ever left without the assistant message that asked for it.
    for (std::size_t max_tokens : {100u, 400u, 1200u, 3000u, 8000u}) {
        auto msgs = original;
        auto mw = context_management({.budget = {.max_tokens = max_tokens, .keep_recent = 4}});
        mw(msgs, [](std::vector<Message>&) {
            return LLMResponse{Message::assistant(""), {}, "stop", {}};
        });
        bool assistant_asked = false;
        for (const auto& m : msgs) {
            if (m.role == Role::tool && !assistant_asked) {
                std::cerr << "\norphaned tool result at budget " << max_tokens << "\n";
                return 1;
            }
            assistant_asked = m.role == Role::assistant && m.has_tool_calls();
        }
    }
    std::cout << "\nEvery budget produced a well-formed conversation.\n";
}
