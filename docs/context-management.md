# Context management

A tool-calling agent fills its context window quickly. Every turn adds an
assistant message, a tool call and a tool result, and the tool results are
usually most of the bytes. Left alone, a long run ends in a
`context_length_exceeded` from the provider.

`context_management()` in `middleware/context.hpp` is the answer: state a token
budget, and when the conversation crosses it, escalate through three techniques
in cost order until it fits.

```cpp
#include <tiny_agent/middleware/context.hpp>

AgentConfig cfg;
cfg.middlewares.push_back(middleware::context_management({
    .budget = {.max_tokens = 8000, .keep_recent = 6}}));
```

Under budget it does nothing, which is the common case and costs one token
count per model call.

## The escalation ladder

| Stage | What it does | Cost | What is lost |
|---|---|---|---|
| 1. Clear tool output | Replaces old tool result bodies with a placeholder, keeping the messages | free | the tool outputs, which the model has already acted on |
| 2. Summarize the middle | Folds everything between the system prompt and the recent tail into one summary | a summarizer call | detail, but the thread survives |
| 3. Drop the oldest | Removes messages from the front | free | information outright |

Each stage runs only if the conversation is still above target after the one
before it. Stage 1 usually suffices: in `examples/20_context_management.cpp`, a
12-turn conversation drops from 3925 to 1035 tokens on blanking tool output
alone, with no message removed and nothing about the conversation's shape
changed.

## The budget

```cpp
struct TokenBudget {
    std::size_t  max_tokens   = 8000;   // compaction starts above this
    double       target_ratio = 0.6;    // compact down to this fraction of max
    std::size_t  keep_recent  = 4;      // tail messages no stage may touch
    bool         keep_system  = true;   // leave the system prompt alone
    TokenCounter count;                 // empty means approx_token_count
};
```

`target_ratio` is why compaction leaves headroom instead of stopping at the
ceiling. Compacting exactly to `max_tokens` means compacting again on the very
next turn; going to 60% buys several turns per pass.

`keep_recent` protects the tail. The model needs the recent turns verbatim to
continue coherently, so no stage will touch them, which means a budget smaller
than the protected tail cannot be met. When that happens the middleware logs a
warning naming the cause and passes the conversation through rather than
destroying it:

```
[WARN] [context] still over budget after compacting (637 > 400 tokens);
       the protected tail alone exceeds it
```

## Counting tokens

The default `approx_token_count` needs no tokenizer: roughly four characters per
token, plus per-message overhead. It counts message text, multimodal parts, and
**tool-call arguments**. Counting text alone misses arguments entirely, so a
tool-heavy conversation, exactly the kind that overruns a window, reads as far
smaller than it is.

For precision, supply a real tokenizer:

```cpp
.budget = {.max_tokens = 8000,
           .count = [&](const std::vector<Message>& msgs) {
               return my_tokenizer.count(msgs);
           }}
```

## Tool-call pairing

Providers reject a `tool` message that does not follow the assistant message
requesting it. OpenAI answers `messages with role 'tool' must be a response to
a preceding message with 'tool_calls'`. A cut that lands in the middle of a
tool-call sequence produces a prompt the API refuses.

Every function here that removes messages snaps its cut point forward past
orphaned tool results, so the surviving conversation is always well formed. That
holds at any budget, including one too small to satisfy;
`tests/test_context.cpp` checks it across a sweep of budgets and every possible
cut point.

This also fixed a live defect: `trim_history` used to cut at a raw index and
could leave a conversation beginning with a tool result.

## Protecting the summary

A summary stands in for every message already folded away, which makes it the
densest thing in the prompt and the worst candidate for the next stage to drop.
Stage 2 marks the message it creates, and stage 3 treats it as protected, so a
run that summarizes and then still has to trim keeps what it paid for.

The marker also stops summaries from accumulating: a later pass folds the
previous summary into the new one instead of stacking a second beside it.

## Choosing the stages

Each stage can be turned off:

```cpp
middleware::context_management({
    .budget = {.max_tokens = 8000},
    .summarize = false,     // never call a summarizer
    .trim = false});        // never drop a message
```

With everything off the middleware is a no-op that still measures, which is a
reasonable way to watch a budget before enforcing it.

## Summarizers

The default is extractive: no model call, no network. It truncates each message
and labels it by role, giving tool results a third of the budget since they are
the most verbose and the least worth quoting. An assistant turn with no text
still reports which tools it called.

To have a model write the summary, pass one in:

```cpp
#include <tiny_agent/middleware/summarize.hpp>

middleware::context_management({
    .budget = {.max_tokens = 8000},
    .summarizer = middleware::LLMSummarizer([&](const std::vector<Message>& msgs) {
        return std::vector<Message>{Message::assistant(
            summarizer_llm.chat(msgs).message.text())};
    })});
```

## The three original middleware

`trim_history`, `summarize` and `context_editing` are still there, unchanged in
behaviour and now built on the same primitives. Each one is a single technique
from the ladder:

| Middleware | Stage | Triggers on |
|---|---|---|
| `context_editing({.trigger = …, .keep = …})` | 1 | a token threshold |
| `summarize({.trigger_tokens = …, .keep_recent = …})` | 2 | a token threshold |
| `trim_history(max_messages)` | 3 | a **message count**, not tokens |

Reach for one of these when you want exactly that technique on its own terms.
Reach for `context_management()` when what you actually have is a token budget,
which is the usual case. Stacking all three means reasoning about three
independent triggers that do not know about each other.

## Primitives

The stages are also available directly, as pure functions on a message vector,
for a policy of your own:

```cpp
namespace tiny_agent::middleware::context {
    bool clear_tool_results(std::vector<Message>&, std::size_t keep,
                            const std::string& placeholder, bool clear_inputs = false);
    bool summarize_middle(std::vector<Message>&, std::size_t keep_recent,
                          const SummarizerFn&, const std::string& prefix,
                          bool keep_system = true);
    bool trim_oldest(std::vector<Message>&, const TokenBudget&);
    bool trim_to_message_count(std::vector<Message>&, std::size_t max_messages,
                               bool keep_system = true);

    std::size_t snap_past_orphan_tools(const std::vector<Message>&, std::size_t cut);
    std::size_t tail_start(const std::vector<Message>&, std::size_t keep_recent);
    bool        is_summary(const Message&);
}
```

Each returns whether it changed anything, so a caller can build its own ladder.

## Example

`examples/20_context_management.cpp` builds a 12-turn tool-calling conversation
and prints what each technique does to it, then runs the full ladder at four
budgets so the escalation is visible. It needs no API key.

```bash
./build/examples/20_context_management
```
