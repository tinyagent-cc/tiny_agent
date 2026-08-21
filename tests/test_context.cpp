#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/middleware/context.hpp>
#include <tiny_agent/middleware/trim_history.hpp>
#include <tiny_agent/middleware/summarize.hpp>
#include <tiny_agent/middleware/context_editing.hpp>
#include <sstream>

using namespace tiny_agent;
using namespace tiny_agent::middleware;

static LLMResponse ok(const std::string& text = "ok") {
    return {Message::assistant(text), {}, "stop", {}};
}

// An assistant turn that calls one tool, plus the tool's reply. Every provider
// requires these two to travel together.
static void push_tool_turn(std::vector<Message>& msgs, const std::string& id,
                           const std::string& name, const std::string& result) {
    Message call = Message::assistant("");
    call.tool_calls.push_back({id, name, json{{"q", "x"}}});
    msgs.push_back(std::move(call));
    auto reply = Message::tool_result(id, result);
    reply.name = name;
    msgs.push_back(std::move(reply));
}

// True when no tool message lacks a preceding assistant turn that requested it.
static bool well_formed(const std::vector<Message>& msgs) {
    bool assistant_with_calls = false;
    for (const auto& m : msgs) {
        if (m.role == Role::tool) {
            if (!assistant_with_calls) return false;
        } else {
            assistant_with_calls = m.role == Role::assistant && m.has_tool_calls();
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Token counting
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("approx_token_count grows with content and counts every message") {
    CHECK(approx_token_count({}) == 0);

    std::vector<Message> one = {Message::user(std::string(400, 'a'))};
    std::vector<Message> two = {Message::user(std::string(400, 'a')),
                                Message::user(std::string(400, 'b'))};
    CHECK(approx_token_count(one) >= 100);
    CHECK(approx_token_count(two) > approx_token_count(one));
}

TEST_CASE("approx_token_count includes tool-call arguments") {
    Message bare = Message::assistant("");
    Message with_args = Message::assistant("");
    with_args.tool_calls.push_back({"id", "search", json{{"query", std::string(800, 'x')}}});

    // Counting only message text would score these identically, and a
    // tool-heavy conversation would read as far smaller than it is.
    CHECK(approx_token_count({with_args}) > approx_token_count({bare}) + 150);
}

TEST_CASE("approx_token_count includes multimodal parts") {
    auto img = Message::image("describe this", std::string(600, 'u'));
    CHECK(approx_token_count({img}) > approx_token_count({Message::user("describe this")}));
}

TEST_CASE("TokenBudget computes a target below its ceiling") {
    TokenBudget b{.max_tokens = 1000, .target_ratio = 0.6};
    CHECK(b.target_tokens() == 600);

    // A nonsense ratio falls back rather than producing a target of zero.
    TokenBudget bad{.max_tokens = 1000, .target_ratio = 0.0};
    CHECK(bad.target_tokens() == 600);
    TokenBudget over{.max_tokens = 1000, .target_ratio = 4.0};
    CHECK(over.target_tokens() == 600);
}

TEST_CASE("TokenBudget uses a supplied counter") {
    TokenBudget b{.max_tokens = 10};
    b.count = [](const std::vector<Message>& m) { return m.size() * 100; };
    std::vector<Message> msgs = {Message::user("tiny")};
    CHECK(b.measure(msgs) == 100);
    CHECK(b.over_budget(msgs));
}

// ═══════════════════════════════════════════════════════════════════════════
// Tool-safe boundaries
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("snap_past_orphan_tools moves a cut off a tool message") {
    std::vector<Message> msgs = {Message::user("q")};
    push_tool_turn(msgs, "t1", "search", "result");
    msgs.push_back(Message::assistant("done"));
    // index:            0=user 1=assistant(calls) 2=tool 3=assistant

    CHECK(context::snap_past_orphan_tools(msgs, 0) == 0);
    CHECK(context::snap_past_orphan_tools(msgs, 1) == 1);
    CHECK(context::snap_past_orphan_tools(msgs, 2) == 3);   // past the orphan
    CHECK(context::snap_past_orphan_tools(msgs, 3) == 3);
    CHECK(context::snap_past_orphan_tools(msgs, msgs.size()) == msgs.size());
}

TEST_CASE("trimming never leaves a conversation starting with a tool result") {
    std::vector<Message> msgs = {Message::system("sys"), Message::user("q")};
    for (int i = 0; i < 6; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", "result " + std::to_string(i));

    // A cut that lands mid tool-call sequence would produce a prompt the
    // provider rejects with "messages with role tool must be a response to a
    // preceding message with tool_calls".
    for (std::size_t limit = 1; limit < msgs.size(); ++limit) {
        auto copy = msgs;
        context::trim_to_message_count(copy, limit);
        CHECK(well_formed(copy));
        CHECK(copy.front().role == Role::system);
    }
}

TEST_CASE("summarizing never orphans a tool result") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 6; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", std::string(600, 'x'));

    for (std::size_t keep = 1; keep < 8; ++keep) {
        auto copy = msgs;
        context::summarize_middle(copy, keep,
            [](const std::vector<Message>&) { return std::string("summary"); },
            "[s]\n");
        CHECK(well_formed(copy));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Stage 1 — clearing tool results
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("clear_tool_results blanks the oldest and keeps the newest") {
    std::vector<Message> msgs;
    for (int i = 0; i < 5; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", "result " + std::to_string(i));

    CHECK(context::clear_tool_results(msgs, 2, "[cleared]"));

    std::vector<std::string> tool_texts;
    for (const auto& m : msgs)
        if (m.role == Role::tool) tool_texts.push_back(m.text());

    REQUIRE(tool_texts.size() == 5);
    CHECK(tool_texts[0] == "[cleared]");
    CHECK(tool_texts[2] == "[cleared]");
    CHECK(tool_texts[3] == "result 3");
    CHECK(tool_texts[4] == "result 4");
    // Messages are kept, only their bodies go, so the pairing survives.
    CHECK(well_formed(msgs));
}

TEST_CASE("clear_tool_results is idempotent") {
    std::vector<Message> msgs;
    for (int i = 0; i < 5; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", "result");

    CHECK(context::clear_tool_results(msgs, 2, "[cleared]"));
    CHECK_FALSE(context::clear_tool_results(msgs, 2, "[cleared]"));
}

TEST_CASE("clear_tool_results does nothing when there is little to clear") {
    std::vector<Message> msgs;
    push_tool_turn(msgs, "t0", "search", "result");
    CHECK_FALSE(context::clear_tool_results(msgs, 3, "[cleared]"));
    CHECK(msgs.back().text() == "result");
}

TEST_CASE("clear_tool_inputs also blanks the arguments") {
    std::vector<Message> msgs;
    for (int i = 0; i < 4; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", "result");

    context::clear_tool_results(msgs, 1, "[cleared]", true);
    CHECK(msgs[0].tool_calls[0].arguments.empty());
    CHECK_FALSE(msgs[6].tool_calls[0].arguments.empty());   // the kept one
}

// ═══════════════════════════════════════════════════════════════════════════
// Stage 2 — summarizing the middle
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("summarize_middle keeps the system prompt and the recent tail") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 8; ++i)
        msgs.push_back(Message::user("message " + std::to_string(i)));

    CHECK(context::summarize_middle(msgs, 3,
        [](const std::vector<Message>& m) {
            return "folded " + std::to_string(m.size()) + " messages";
        }, "[Summary]\n"));

    REQUIRE(msgs.size() == 5);                    // system + summary + 3 recent
    CHECK(msgs[0].text() == "sys");
    CHECK(msgs[1].role == Role::system);
    CHECK(msgs[1].text() == "[Summary]\nfolded 5 messages");
    CHECK(msgs[2].text() == "message 5");
    CHECK(msgs[4].text() == "message 7");
}

TEST_CASE("summarize_middle declines when there is nothing to fold") {
    std::vector<Message> msgs = {Message::system("sys"), Message::user("only one")};
    CHECK_FALSE(context::summarize_middle(msgs, 4,
        [](const std::vector<Message>&) { return std::string("s"); }, "[S]\n"));
    CHECK(msgs.size() == 2);
}

TEST_CASE("summarize_middle declines an empty summary rather than losing history") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 8; ++i) msgs.push_back(Message::user("m" + std::to_string(i)));
    auto before = msgs.size();

    CHECK_FALSE(context::summarize_middle(msgs, 2,
        [](const std::vector<Message>&) { return std::string(); }, "[S]\n"));
    CHECK(msgs.size() == before);
}

TEST_CASE("extractive_summarize labels roles and describes tool calls") {
    std::vector<Message> msgs;
    msgs.push_back(Message::user("what is the weather"));
    push_tool_turn(msgs, "t0", "get_weather", std::string(400, 'x'));

    auto summary = extractive_summarize(msgs, 60);
    CHECK(summary.find("user: what is the weather") != std::string::npos);
    // An assistant turn with no text still says what it did.
    CHECK(summary.find("called get_weather") != std::string::npos);
    // Tool output is clipped hardest, at a third of the budget.
    CHECK(summary.find(std::string(30, 'x')) == std::string::npos);
    CHECK(summary.find("...") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// Stage 3 — trimming
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("trim_oldest drops from the front until it reaches the target") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 20; ++i) msgs.push_back(Message::user(std::string(400, 'a')));

    // Target of 500 is reachable: the 3-message protected tail is about 316.
    TokenBudget budget{.max_tokens = 1000, .target_ratio = 0.5, .keep_recent = 3};
    CHECK(context::trim_oldest(msgs, budget));

    CHECK(msgs.front().role == Role::system);
    CHECK(msgs.size() >= 4);                       // system plus the protected tail
    CHECK(budget.measure(msgs) <= budget.target_tokens());
}

TEST_CASE("trim_oldest stops once only the protected tail is left") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 20; ++i) msgs.push_back(Message::user(std::string(400, 'a')));

    // Target below what the tail alone costs: trim must give up, not overrun.
    TokenBudget budget{.max_tokens = 400, .target_ratio = 0.5, .keep_recent = 3};
    context::trim_oldest(msgs, budget);
    CHECK(msgs.size() == 4);                       // system plus three recent
    CHECK(budget.measure(msgs) > budget.target_tokens());
}

TEST_CASE("trim_oldest refuses to eat the protected tail") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 3; ++i) msgs.push_back(Message::user(std::string(4000, 'a')));

    // The tail alone blows the budget; trimming must stop rather than destroy it.
    TokenBudget budget{.max_tokens = 10, .keep_recent = 3};
    context::trim_oldest(msgs, budget);
    CHECK(msgs.size() == 4);
    CHECK(budget.over_budget(msgs));
}

TEST_CASE("trim_to_message_count respects the message limit") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 10; ++i) msgs.push_back(Message::user("m" + std::to_string(i)));

    CHECK(context::trim_to_message_count(msgs, 4));
    CHECK(msgs.size() == 5);                       // system is not counted
    CHECK(msgs[0].role == Role::system);
    CHECK(msgs[1].text() == "m6");
}

TEST_CASE("trim_to_message_count leaves a short conversation alone") {
    std::vector<Message> msgs = {Message::user("a"), Message::user("b")};
    CHECK_FALSE(context::trim_to_message_count(msgs, 10));
    CHECK(msgs.size() == 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// context_management — the ladder
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("context_management does nothing while under budget") {
    auto mw = context_management({.budget = {.max_tokens = 100'000}});
    std::vector<Message> msgs = {Message::system("sys"), Message::user("short")};
    auto before = msgs;

    auto r = mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(r.finish_reason == "stop");
    CHECK(msgs.size() == before.size());
    CHECK(msgs[1].text() == "short");
}

TEST_CASE("context_management stops at stage one when that is enough") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 8; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", std::string(1000, 'x'));

    bool summarizer_ran = false;
    auto mw = context_management({
        .budget = {.max_tokens = 800, .target_ratio = 0.9, .keep_recent = 4},
        .keep_tool_results = 2,
        .summarizer = [&](const std::vector<Message>&) {
            summarizer_ran = true; return std::string("summary");
        }});

    auto before = msgs.size();
    mw(msgs, [](std::vector<Message>&) { return ok(); });

    // Blanking tool output alone got it under target, so nothing was dropped.
    CHECK(msgs.size() == before);
    CHECK_FALSE(summarizer_ran);
    CHECK(well_formed(msgs));
}

TEST_CASE("context_management escalates to summarizing when clearing is not enough") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 12; ++i) msgs.push_back(Message::user(std::string(2000, 'a')));

    bool summarizer_ran = false;
    auto mw = context_management({
        .budget = {.max_tokens = 500, .target_ratio = 0.6, .keep_recent = 3},
        .summarizer = [&](const std::vector<Message>&) {
            summarizer_ran = true; return std::string("the middle, condensed");
        }});

    mw(msgs, [](std::vector<Message>&) { return ok(); });

    CHECK(summarizer_ran);
    CHECK(msgs[0].text() == "sys");
    bool found_summary = false;
    for (const auto& m : msgs)
        if (m.text().find("the middle, condensed") != std::string::npos) found_summary = true;
    CHECK(found_summary);
}

TEST_CASE("context_management trims when the earlier stages are unavailable") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 12; ++i) msgs.push_back(Message::user(std::string(400, 'a')));
    auto before = msgs.size();

    auto mw = context_management({
        .budget = {.max_tokens = 1000, .target_ratio = 0.5, .keep_recent = 3},
        .summarize = false});          // leaves trimming as the only stage

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(msgs.size() < before);
    CHECK(msgs.front().role == Role::system);
}

TEST_CASE("a summary survives the trimming stage that follows it") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 12; ++i) msgs.push_back(Message::user(std::string(2000, 'a')));

    // The tail alone busts this budget, so trimming runs after summarizing.
    // Dropping the summary would discard everything it stands for.
    auto mw = context_management({
        .budget = {.max_tokens = 200, .target_ratio = 0.5, .keep_recent = 2},
        .summarizer = [](const std::vector<Message>&) {
            return std::string("the middle, condensed");
        }});

    mw(msgs, [](std::vector<Message>&) { return ok(); });

    bool found_summary = false;
    for (const auto& m : msgs)
        if (m.text().find("the middle, condensed") != std::string::npos) found_summary = true;
    CHECK(found_summary);
    CHECK(msgs.front().text() == "sys");
}

TEST_CASE("repeated compaction does not stack up summaries") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 12; ++i) msgs.push_back(Message::user(std::string(2000, 'a')));

    auto mw = context_management({
        .budget = {.max_tokens = 400, .target_ratio = 0.5, .keep_recent = 2},
        .summarizer = [](const std::vector<Message>&) { return std::string("condensed"); }});

    for (int pass = 0; pass < 3; ++pass) {
        mw(msgs, [](std::vector<Message>&) { return ok(); });
        msgs.push_back(Message::user(std::string(2000, 'b')));   // the next turn
    }

    std::size_t summaries = 0;
    for (const auto& m : msgs) if (context::is_summary(m)) ++summaries;
    CHECK(summaries <= 1);
}

TEST_CASE("context_management can run a single stage") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 10; ++i) msgs.push_back(Message::user(std::string(2000, 'a')));
    auto before = msgs.size();

    auto mw = context_management({
        .budget = {.max_tokens = 100, .keep_recent = 2},
        .clear_tool_results = false,
        .summarize = false,
        .trim = false});

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(msgs.size() == before);   // every stage disabled: nothing happens
}

TEST_CASE("context_management warns when the protected tail alone exceeds the budget") {
    std::ostringstream sink;
    std::vector<Message> msgs = {Message::user(std::string(8000, 'a')),
                                 Message::user(std::string(8000, 'b'))};

    auto mw = context_management({
        .budget = {.max_tokens = 50, .keep_recent = 4},
        .log = Log{sink, LogLevel::warn}});

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(sink.str().find("still over budget") != std::string::npos);
}

TEST_CASE("context_management keeps the conversation well formed under pressure") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 10; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", std::string(1500, 'x'));

    for (std::size_t max_tokens : {50u, 200u, 800u, 2000u}) {
        auto copy = msgs;
        auto mw = context_management({.budget = {.max_tokens = max_tokens, .keep_recent = 3}});
        mw(copy, [](std::vector<Message>&) { return ok(); });
        CHECK(well_formed(copy));
        CHECK(copy.front().role == Role::system);
    }
}

TEST_CASE("context_management passes the compacted messages to the model") {
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 12; ++i) msgs.push_back(Message::user(std::string(2000, 'a')));

    std::size_t seen_by_model = 0;
    auto mw = context_management({.budget = {.max_tokens = 300, .keep_recent = 2}});
    mw(msgs, [&](std::vector<Message>& m) { seen_by_model = m.size(); return ok(); });

    CHECK(seen_by_model == msgs.size());
    CHECK(seen_by_model < 13);
}

// ═══════════════════════════════════════════════════════════════════════════
// The three original middleware still behave
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("trim_history still enforces a message count") {
    auto mw = trim_history(3);
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 10; ++i) msgs.push_back(Message::user("m" + std::to_string(i)));

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(msgs.size() == 4);
    CHECK(msgs[0].role == Role::system);
}

TEST_CASE("trim_history no longer orphans a tool result") {
    auto mw = trim_history(3);
    std::vector<Message> msgs = {Message::system("sys"), Message::user("q")};
    for (int i = 0; i < 4; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", "result");

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(well_formed(msgs));
}

TEST_CASE("context_editing still triggers on its own threshold") {
    auto mw = context_editing({.trigger = 100, .keep = 1, .placeholder = "[gone]"});
    std::vector<Message> msgs;
    for (int i = 0; i < 4; ++i)
        push_tool_turn(msgs, "t" + std::to_string(i), "search", std::string(400, 'x'));

    mw(msgs, [](std::vector<Message>&) { return ok(); });

    std::vector<std::string> texts;
    for (const auto& m : msgs) if (m.role == Role::tool) texts.push_back(m.text());
    REQUIRE(texts.size() == 4);
    CHECK(texts[0] == "[gone]");
    CHECK(texts[3].size() == 400);
}

TEST_CASE("context_editing leaves a short conversation alone") {
    auto mw = context_editing({.trigger = 100'000});
    std::vector<Message> msgs;
    push_tool_turn(msgs, "t0", "search", "result");
    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(msgs.back().text() == "result");
}

TEST_CASE("summarize still folds the middle on its own trigger") {
    auto mw = summarize({.trigger_tokens = 100, .keep_recent = 2});
    std::vector<Message> msgs = {Message::system("sys")};
    for (int i = 0; i < 10; ++i) msgs.push_back(Message::user(std::string(200, 'a')));

    mw(msgs, [](std::vector<Message>&) { return ok(); });
    CHECK(msgs.size() == 4);                    // system + summary + 2 recent
    CHECK(msgs[1].text().find("[Conversation summary]") == 0);
}
