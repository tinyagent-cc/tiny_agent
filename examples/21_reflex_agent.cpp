// 21_reflex_agent — rete rules answering the easy cases before the model runs
//
// Pairs tiny_agent's ReflexEngine with a Rete rule base to handle two things
// deterministically and for free: a message that matches a rule short-circuits
// the model entirely (a reflex, zero tokens spent), and after the model runs,
// a guardrail rule can veto or rewrite a tool call it made.
//
// This example needs no API key or network — a stub stands in for the model
// so the demo runs anywhere.
//
//   ./build/examples/21_reflex_agent

#include <tiny_agent/middleware/reflex.hpp>
#include <tiny_agent/tiny_agent.hpp>
#include <iostream>

using namespace tiny_agent;

int main() {
    middleware::ReflexEngine rx;

    // A reflex: "ping" never reaches the model.
    rx.engine().add_rule("ping")
        .when("msg", "text", "ping")
        .then([&](auto&, auto&) { rx.outcome().respond("pong"); })
        .build();

    // A guardrail: a destructive tool call gets vetoed after the model
    // proposes it. The `?c` variable matches any call identifier, so this one
    // rule covers call-0, call-1, ... — but veto(0, ...) hardcodes the index.
    // A response with more than one tool call would need the real index
    // derived from the binding instead.
    rx.engine().add_rule("no-delete")
        .when("?c", "tool", "delete_everything")
        .then([&](auto&, auto&) { rx.outcome().veto(0, "destructive tool blocked"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_facts = middleware::message_facts,
        .extract_response_facts = middleware::tool_call_facts,
    }));

    // Stands in for the model: always proposes a destructive tool call, so
    // the guardrail path below has something to veto.
    int model_calls = 0;
    auto stub_model = [&](std::vector<Message>&) -> LLMResponse {
        ++model_calls;
        Message m = Message::assistant("");
        m.tool_calls.push_back({"call-0", "delete_everything", {{"path", "/"}}});
        return {std::move(m), {}, "tool_calls", {}};
    };

    // Path 1: the reflex answers directly. The model is never called.
    std::vector<Message> reflex_msgs = {Message::user("ping")};
    auto reflex_resp = chain.run(reflex_msgs, stub_model);
    std::cout << "reflex path\n"
              << "  message:     \"" << reflex_msgs.back().text() << "\"\n"
              << "  answer:      \"" << reflex_resp.message.text() << "\"\n"
              << "  finish:      " << reflex_resp.finish_reason << "\n"
              << "  model calls: " << model_calls << "\n\n";

    // Path 2: nothing matches the message, so the model runs and proposes a
    // tool call — which the guardrail rule then blocks.
    std::vector<Message> guard_msgs = {Message::user("clean up the workspace")};
    auto guard_resp = chain.run(guard_msgs, stub_model);
    std::cout << "guardrail path\n"
              << "  message:      \"" << guard_msgs.back().text() << "\"\n"
              << "  answer:       \"" << guard_resp.message.text() << "\"\n"
              << "  finish:       " << guard_resp.finish_reason << "\n"
              << "  model calls:  " << model_calls << "\n"
              << "  tool calls left: " << guard_resp.message.tool_calls.size() << "\n"
              << "  veto log:     " << guard_resp.raw.value("reflex_vetoes", json::array()).dump() << "\n";

    return 0;
}
