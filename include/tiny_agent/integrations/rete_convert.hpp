#pragma once
#include "../core/types.hpp"
#include <rete/types.hpp>

namespace tiny_agent::integrations {

inline rete::Value to_rete_value(const json& j) {
    if (j.is_null())            return std::monostate{};
    if (j.is_boolean())         return j.get<bool>();
    if (j.is_number_integer())  return j.get<int64_t>();
    if (j.is_number_unsigned()) return static_cast<int64_t>(j.get<uint64_t>());
    if (j.is_number_float())    return j.get<double>();
    if (j.is_string())          return j.get<std::string>();
    return j.dump();
}

inline json from_rete_value(const rete::Value& v) {
    return std::visit([](auto&& arg) -> json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) return json();
        else return json(arg);
    }, v);
}

} // namespace tiny_agent::integrations
