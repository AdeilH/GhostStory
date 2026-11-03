// addu94 — src/core/toml_min.cpp
#include "core/toml_min.hpp"

#include <cctype>

namespace addu94::core {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parse_scalar(std::string_view raw, TomlValue& out) {
    raw = trim(raw);
    if (raw.empty()) return false;
    if (raw.front() == '"' && raw.back() == '"' && raw.size() >= 2) {
        out.type = TomlValue::Type::String;
        out.str = std::string{raw.substr(1, raw.size() - 2)};
        return true;
    }
    if (raw == "true") {
        out.type = TomlValue::Type::Bool;
        out.b = true;
        return true;
    }
    if (raw == "false") {
        out.type = TomlValue::Type::Bool;
        out.b = false;
        return true;
    }
    // Number: allow leading +/-, digits, one dot, one exponent.
    std::size_t i = 0;
    if (i < raw.size() && (raw[i] == '+' || raw[i] == '-')) ++i;
    bool digits = false, dot = false, bad = false;
    for (; i < raw.size(); ++i) {
        char const c = raw[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits = true;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            bad = true;
            break;
        }
    }
    if (bad || !digits) return false;
    if (dot) {
        out.type = TomlValue::Type::Float;
        out.f = std::stod(std::string{raw});
    } else {
        out.type = TomlValue::Type::Int;
        out.i = std::stoll(std::string{raw});
    }
    return true;
}

}  // namespace

std::optional<TomlTable> toml_parse(std::string_view text, TomlError& err) {
    TomlTable table;
    std::string section;
    int line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto const eol = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, eol == std::string_view::npos ? std::string_view::npos
                                                           : eol - pos);
        ++line_no;
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;
        // strip comments (naive: no # inside our quoted strings by policy)
        auto const hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                err = {line_no, "unterminated section header"};
                return std::nullopt;
            }
            section = std::string{trim(line.substr(1, line.size() - 2))};
            if (section.empty() || section.find('.') != std::string_view::npos ||
                section.find('[') != std::string_view::npos) {
                err = {line_no, "unsupported section name"};
                return std::nullopt;
            }
            continue;
        }

        auto const eq = line.find('=');
        if (eq == std::string_view::npos) {
            err = {line_no, "expected key = value"};
            return std::nullopt;
        }
        auto const key = trim(line.substr(0, eq));
        if (key.empty() || key.find(' ') != std::string_view::npos) {
            err = {line_no, "bad key"};
            return std::nullopt;
        }
        TomlValue value;
        if (!parse_scalar(line.substr(eq + 1), value)) {
            err = {line_no, "bad value (allowed: \"string\", int, float, bool)"};
            return std::nullopt;
        }
        std::string const full =
            section.empty() ? std::string{key} : section + "." + std::string{key};
        table[full] = std::move(value);
        if (eol == std::string_view::npos) break;
    }
    return table;
}

}  // namespace addu94::core
