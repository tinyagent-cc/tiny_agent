#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  runnable.hpp  —  Core Runnable concept + composition primitives
//
//  Mirrors LangChain's Runnable[Input, Output] interface.
//    Runnable<T, I, O>   — concept: anything that can invoke/batch/stream
//    RunnableLambda      — wraps any callable into a Runnable
//    RunnableSequence    — chains N runnables via parameter pack; supports |
//    RunnableParallel    — fan-out to N runnables, collects into tuple
// ═══════════════════════════════════════════════════════════════════════════════

#include <concepts>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace tiny_agent {

struct RunConfig {
    std::string             run_name   {};
    std::vector<std::string> tags      {};
    std::size_t             max_concurrency { 0 };
};

template<typename T, typename Input, typename Output>
concept Runnable =
    requires { typename T::input_t; typename T::output_t; } &&
    std::same_as<typename T::input_t,  Input>               &&
    std::same_as<typename T::output_t, Output>              &&
    requires(T r,
             Input                             in,
             std::vector<Input>               batch_in,
             std::function<void(Output)>      cb,
             const RunConfig&                 cfg)
    {
        { r.invoke(in) }         -> std::convertible_to<Output>;
        { r.batch(batch_in) }    -> std::convertible_to<std::vector<Output>>;
        { r.stream(in, cb) }     -> std::same_as<void>;
    };

// ─── RunnableLambda ───────────────────────────────────────────────────────────

template<typename Fn, typename Input, typename Output>
class RunnableLambda {
    Fn fn_;
public:
    using input_t  = Input;
    using output_t = Output;

    explicit RunnableLambda(Fn fn) : fn_(std::move(fn)) {}

    Output invoke(Input in, const RunConfig& = {}) {
        return fn_(std::move(in));
    }

    std::vector<Output> batch(std::vector<Input> inputs, const RunConfig& cfg = {}) {
        std::vector<Output> out;
        out.reserve(inputs.size());
        for (auto& i : inputs) out.push_back(invoke(std::move(i), cfg));
        return out;
    }

    void stream(Input in, std::function<void(Output)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(in), cfg));
    }
};

template<typename Input, typename Output, typename Fn>
auto make_runnable(Fn&& fn) {
    return RunnableLambda<std::decay_t<Fn>, Input, Output>(std::forward<Fn>(fn));
}

// ─── RunnableSequence ─────────────────────────────────────────────────────────

namespace detail {

template<typename Head, typename... Tail>
struct last_output { using type = typename last_output<Tail...>::type; };
template<typename Head>
struct last_output<Head> { using type = typename Head::output_t; };

template<typename Tuple, std::size_t I = 0>
auto thread_steps(Tuple& steps, auto val) {
    if constexpr (I + 1 == std::tuple_size_v<Tuple>) {
        return std::get<I>(steps).invoke(std::move(val));
    } else {
        return thread_steps<Tuple, I+1>(steps,
                   std::get<I>(steps).invoke(std::move(val)));
    }
}

} // namespace detail

template<typename First, typename... Rest>
class RunnableSequence {
    std::tuple<First, Rest...> steps_;

public:
    using input_t  = typename First::input_t;
    using output_t = typename detail::last_output<First, Rest...>::type;

    explicit RunnableSequence(First first, Rest... rest)
        : steps_(std::move(first), std::move(rest)...) {}

    output_t invoke(input_t in, const RunConfig& = {}) {
        return detail::thread_steps(steps_, std::move(in));
    }

    std::vector<output_t> batch(std::vector<input_t> inputs, const RunConfig& cfg = {}) {
        std::vector<output_t> out;
        out.reserve(inputs.size());
        for (auto& i : inputs) out.push_back(invoke(std::move(i), cfg));
        return out;
    }

    void stream(input_t in, std::function<void(output_t)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(in), cfg));
    }
};

template<typename First, typename... Rest>
RunnableSequence(First, Rest...) -> RunnableSequence<First, Rest...>;

template<typename A, typename B>
    requires std::same_as<typename A::output_t, typename B::input_t>
auto operator|(A a, B b) -> RunnableSequence<A, B> {
    return RunnableSequence<A, B>(std::move(a), std::move(b));
}

// ─── RunnableParallel ─────────────────────────────────────────────────────────

template<typename... Branches>
class RunnableParallel {
    std::tuple<Branches...> branches_;

    template<std::size_t... Is>
    auto invoke_all(const typename std::tuple_element_t<0,decltype(branches_)>::input_t& in,
                    std::index_sequence<Is...>) {
        return std::make_tuple(std::get<Is>(branches_).invoke(in)...);
    }

public:
    using input_t  = typename std::tuple_element_t<0, std::tuple<Branches...>>::input_t;
    using output_t = std::tuple<typename Branches::output_t...>;

    explicit RunnableParallel(Branches... b) : branches_(std::move(b)...) {}

    output_t invoke(input_t in, const RunConfig& = {}) {
        return invoke_all(in, std::index_sequence_for<Branches...>{});
    }

    std::vector<output_t> batch(std::vector<input_t> inputs, const RunConfig& cfg = {}) {
        std::vector<output_t> out;
        out.reserve(inputs.size());
        for (auto& i : inputs) out.push_back(invoke(i, cfg));
        return out;
    }

    void stream(input_t in, std::function<void(output_t)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(in), cfg));
    }
};

template<typename... Branches>
auto make_parallel(Branches... b) {
    return RunnableParallel<Branches...>(std::move(b)...);
}

} // namespace tiny_agent
