// addu94 — tests/test_toml.cpp
// Parser + fuzz-lite: garbage never crashes.
#include "test_framework.hpp"

#include "core/toml_min.hpp"

#include <string>
#include <vector>

using addu94::core::TomlError;
using addu94::core::TomlTable;
using addu94::core::toml_parse;

TEST(toml, parses_all_scalar_types) {
    TomlError err;
    auto t = toml_parse(R"(# comment
title = "addu94"
tempo = 120
gain = 0.75
enabled = true
[audio]
buffer = 512
rate = 48000.5
)", err);
    CHECK(t.has_value());
    CHECK_EQ(t->at("title").as_str(), "addu94");
    CHECK_EQ(t->at("tempo").as_int(), 120);
    CHECK(t->at("tempo").as_float() == 120.0);  // int promotes
    CHECK(t->at("gain").as_float() == 0.75);
    CHECK(t->at("enabled").as_bool());
    CHECK_EQ(t->at("audio.buffer").as_int(), 512);
    CHECK(t->at("audio.rate").as_float() == 48000.5);
}

TEST(toml, rejects_garbage_deterministically) {
    TomlError err;
    CHECK(!toml_parse("key = ", err).has_value());
    CHECK(!toml_parse("no equals sign", err).has_value());
    CHECK(!toml_parse("[unterminated", err).has_value());
    CHECK(!toml_parse("x = [1,2]", err).has_value());  // arrays unsupported
    CHECK(!toml_parse("= 5", err).has_value());
    CHECK(!toml_parse("a b = 5", err).has_value());
}

TEST(toml, fuzz_lite_random_bytes_never_crash) {
    // Deterministic PRNG byte salad — parser must return a value or error.
    unsigned seed = 99;
    TomlError err;
    for (int iter = 0; iter < 500; ++iter) {
        std::string s;
        std::size_t const len = seed % 60;
        for (std::size_t i = 0; i < len; ++i) {
            seed = seed * 1103515245u + 12345u;
            s += static_cast<char>(32 + (seed % 95));
        }
        auto const r = toml_parse(s, err);
        CHECK(r.has_value() || !r.has_value());  // exercised; no crash
    }
}

TEST_MAIN()
