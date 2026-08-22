#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rete/rete.hpp>
#include <tiny_agent/integrations/rete_convert.hpp>

TEST_CASE("rete_cpp compiles under the C++20 test build and fires a rule") {
    rete::ReteEngine eng;
    int fired = 0;
    eng.add_rule("smoke")
        .when(std::string("x"), std::string("is"), std::string("on"))
        .then([&](rete::ReteEngine&, const rete::Bindings&) { ++fired; })
        .build();
    eng.assert_fact(std::string("x"), std::string("is"), std::string("on"));
    eng.run();
    CHECK(fired == 1);
}

TEST_CASE("json <-> rete::Value round-trips scalars") {
    using tiny_agent::integrations::to_rete_value;
    using tiny_agent::integrations::from_rete_value;
    using json = nlohmann::json;

    CHECK(std::get<std::string>(to_rete_value(json("hi"))) == "hi");
    CHECK(std::get<int64_t>(to_rete_value(json(42))) == 42);
    CHECK(std::get<double>(to_rete_value(json(2.5))) == 2.5);
    CHECK(std::get<bool>(to_rete_value(json(true))) == true);
    CHECK(std::holds_alternative<std::monostate>(to_rete_value(json())));
    // non-scalar collapses to its compact dump, so no fact is ever lost silently
    CHECK(std::get<std::string>(to_rete_value(json::parse(R"({"a":1})"))) == R"({"a":1})");

    CHECK(from_rete_value(rete::Value{std::string("hi")}) == json("hi"));
    CHECK(from_rete_value(rete::Value{int64_t{42}}) == json(42));
    CHECK(from_rete_value(rete::Value{true}) == json(true));
    CHECK(from_rete_value(rete::Value{std::monostate{}}) == json());
}
