// addu94 — src/core/toml_min.hpp
//
// Minimal TOML-subset parser for OUR OWN config files (config.toml,
// profile.toml, rules.toml). Supports: [sections], comments (#), key =
// value with string ("..."), integer, float, bool. No arrays/inline
// tables/dates — add when a real need appears. Everything stdlib; returns
// errors as a path+message, never throws on user input.
//
// Deterministic and fuzz-target: garbage in → error or valid tree, never
// a crash (M7 fuzz harness will hold us to that).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace addu94::core {

struct TomlValue {
    enum class Type : std::uint8_t { String, Int, Float, Bool } type{Type::String};
    std::string str;
    std::int64_t i{0};
    double f{0.0};
    bool b{false};

    [[nodiscard]] std::string const& as_str() const { return str; }
    [[nodiscard]] std::int64_t as_int(std::int64_t dflt = 0) const {
        return type == Type::Int ? i : dflt;
    }
    [[nodiscard]] double as_float(double dflt = 0.0) const {
        return type == Type::Float ? f
             : type == Type::Int   ? static_cast<double>(i)
                                   : dflt;
    }
    [[nodiscard]] bool as_bool(bool dflt = false) const {
        return type == Type::Bool ? b : dflt;
    }
};

/// key = "section.key" (single nesting level is enough for our configs).
using TomlTable = std::map<std::string, TomlValue>;

struct TomlError {
    int line{0};
    std::string message;
};

/// Parse text; returns nullopt + error on malformed input.
std::optional<TomlTable> toml_parse(std::string_view text, TomlError& err);

}  // namespace addu94::core
