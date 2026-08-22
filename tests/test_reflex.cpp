#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rete/rete.hpp>

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
