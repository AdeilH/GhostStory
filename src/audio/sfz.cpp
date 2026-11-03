// addu94 — src/audio/sfz.cpp
//
// SFZ v1 practical-subset parser implementing the contract in sfz.hpp.
//
// Parsing strategy (house style, cf. core/toml_min.cpp): read the whole
// (size-capped) text file, tokenize, classify each token as header
// (<...>), opcode (key=value) or garbage; never throw on user input —
// malformed content increments a skip counter and moves on. Region final
// sample paths are resolved here so callers get absolute-or-base-relative
// paths that are safe to hand to wav_load directly.

#include "audio/sfz.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace addu94::audio {

namespace {

// SFZ text files are small; anything bigger is hostile input.
constexpr std::size_t kSfzTextCap = 16u * 1024u * 1024u;  // 16 MB

std::string ascii_lower(std::string s) noexcept {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

int clamp_int(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clamp_float(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Strict integer: optional sign, digits only, bounded (rejects overflow,
/// "12abc", "").
bool parse_int(std::string const& s, int& out) noexcept {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    if (i >= s.size()) {
        return false;
    }
    long v = 0;
    for (; i < s.size(); ++i) {
        char const c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
        if (v > 1000000L) {
            return false;
        }
    }
    out = clamp_int(static_cast<int>(neg ? -v : v), -1000000, 1000000);
    return true;
}

/// Strict float (toml_min-style): sign, digits, one optional dot + digits.
/// Rejects "", ".", "-", "1e5" is NOT accepted (SFZ gain values never use
/// exponents in practice).
bool parse_float(std::string const& s, float& out) noexcept {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    bool digits = false;
    bool dot = false;
    double v = 0.0;
    double frac = 0.1;
    for (; i < s.size(); ++i) {
        char const c = s[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            if (!dot) {
                v = v * 10.0 + static_cast<double>(c - '0');
            } else {
                v += static_cast<double>(c - '0') * frac;
                frac *= 0.1;
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits) {
        return false;
    }
    out = static_cast<float>(neg ? -v : v);
    return true;
}

/// Tokenize SFZ text: whitespace-separated; '//' at a token boundary starts
/// a comment to end of line; '"' inside a token toggles quoting so quoted
/// sample paths may contain spaces (quotes are dropped).
std::vector<std::string> tokenize(std::string const& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        char const c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        std::string tok;
        bool in_quote = false;
        while (i < text.size()) {
            char const d = text[i];
            if (!in_quote &&
                (d == ' ' || d == '\t' || d == '\r' || d == '\n')) {
                break;
            }
            if (d == '"') {  // quotes are syntax, not content
                in_quote = !in_quote;
                ++i;
                continue;
            }
            tok += d;
            ++i;
        }
        if (!tok.empty()) {
            out.push_back(std::move(tok));
        }
    }
    return out;
}

bool is_header(std::string const& tok, std::string const& name) noexcept {
    if (tok.size() != name.size() + 2 || tok.front() != '<' ||
        tok.back() != '>') {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = tok[i + 1];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

/// Directory part of `path` (everything up to the last '/'; "" if none).
std::string dir_of(std::string const& path) noexcept {
    std::size_t const slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash);
}

/// Security check: reject any path with a ".." component (path traversal
/// out of the SFZ directory). `raw` uses '/' separators.
bool path_escapes_base(std::string const& raw) noexcept {
    std::size_t i = 0;
    while (i <= raw.size()) {
        std::size_t const next = raw.find('/', i);
        std::size_t const end =
            next == std::string::npos ? raw.size() : next;
        if (end - i == 2 && raw[i] == '.' && raw[i + 1] == '.') {
            return true;
        }
        if (next == std::string::npos) {
            break;
        }
        i = next + 1;
    }
    return false;
}

/// Resolve a sample value to a usable path: '/' separators, absolute paths
/// kept, relative paths joined to the SFZ directory. Returns "" when the
/// value is empty or escapes the base directory (caller skips the region).
std::string resolve_sample(std::string const& base_dir,
                           std::string const& raw_value) noexcept {
    if (raw_value.empty()) {
        return {};
    }
    std::string p;
    p.reserve(raw_value.size());
    for (char const c : raw_value) {
        p += (c == '\\') ? '/' : c;
    }
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    if (path_escapes_base(p)) {
        return {};
    }
    if (!p.empty() && p.front() == '/') {
        return p;  // absolute
    }
    return base_dir.empty() ? p : base_dir + "/" + p;
}

/// Apply one opcode to `r`. Unknown keys return false (caller ignores).
bool apply_opcode(SfzRegion& r, std::string const& key,
                  std::string const& value) noexcept {
    if (key == "sample") {
        // Resolution to an absolute/base-relative path happens at region
        // finalization (needs the SFZ dir); store the raw value for now.
        r.sample_path = value;
        return true;
    }
    int iv = 0;
    float fv = 0.f;
    if (key == "key") {
        if (!parse_int(value, iv)) return true;  // bad value: ignore opcode
        iv = clamp_int(iv, 0, 127);
        r.key = iv;
        r.lo_key = iv;
        r.hi_key = iv;
        return true;
    }
    if (key == "lokey") {
        if (parse_int(value, iv)) r.lo_key = clamp_int(iv, 0, 127);
        return true;
    }
    if (key == "hikey") {
        if (parse_int(value, iv)) r.hi_key = clamp_int(iv, 0, 127);
        return true;
    }
    if (key == "lovel") {
        if (parse_int(value, iv)) r.lo_vel = clamp_int(iv, 0, 127);
        return true;
    }
    if (key == "hivel") {
        if (parse_int(value, iv)) r.hi_vel = clamp_int(iv, 0, 127);
        return true;
    }
    if (key == "pitch_keycenter") {
        if (parse_int(value, iv)) r.pitch_keycenter = clamp_int(iv, 0, 127);
        return true;
    }
    if (key == "transpose") {
        if (parse_int(value, iv)) r.transpose = clamp_int(iv, -96, 96);
        return true;
    }
    if (key == "volume") {
        if (parse_float(value, fv)) {
            r.volume_db = clamp_float(fv, -90.f, 12.f);
        }
        return true;
    }
    if (key == "tune") {
        if (parse_float(value, fv)) {
            r.tune_cents = clamp_float(fv, -100.f, 100.f);
        }
        return true;
    }
    return false;  // unknown opcode: ignored
}

/// Push a finished region into `res`; counts skips for unusable regions.
void finalize_region(SfzParseResult& res, SfzRegion&& region,
                     std::string const& base_dir) {
    if (region.sample_path.empty()) {
        // A <region> with no sample is unusable (an empty <region> header
        // pair or a missing `sample` opcode).
        ++res.skipped_regions;
        return;
    }
    std::string const resolved = resolve_sample(base_dir, region.sample_path);
    if (resolved.empty()) {
        ++res.skipped_regions;  // ".." escape or empty after normalization
        return;
    }
    if (region.lo_key > region.hi_key) {
        ++res.skipped_regions;  // inverted range: malformed
        return;
    }
    if (region.lo_vel > region.hi_vel) {
        ++res.skipped_regions;
        return;
    }
    region.sample_path = resolved;
    res.regions.push_back(std::move(region));
}

std::string read_text_file(std::string const& path, std::string& error,
                           bool& ok) noexcept {
    ok = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "sfz: cannot open " + path;
        return {};
    }
    std::string text;
    char buf[8192];
    bool too_big = false;
    for (;;) {
        std::size_t const n = std::fread(buf, 1, sizeof buf, f);
        if (n > 0) {
            if (text.size() + n > kSfzTextCap) {
                too_big = true;
                break;
            }
            text.append(buf, n);
        }
        if (n < sizeof buf) {
            if (std::ferror(f) != 0) {
                std::fclose(f);
                error = "sfz: read error on " + path;
                return {};
            }
            break;
        }
    }
    std::fclose(f);
    if (too_big) {
        error = "sfz: file exceeds 16 MB cap: " + path;
        return {};
    }
    ok = true;
    return text;
}

}  // namespace

std::optional<SfzParseResult> sfz_load(std::string const& path,
                                       std::string& error) {
    error.clear();
    bool ok = false;
    std::string text;
    try {
        text = read_text_file(path, error, ok);
    } catch (std::exception const& e) {
        error = std::string{"sfz: read failed: "} + e.what();
        return std::nullopt;
    }
    if (!ok) {
        return std::nullopt;
    }

    SfzParseResult res;
    try {
        std::vector<std::string> const tokens = tokenize(text);
        SfzRegion group_defaults{};  // <group>/<global> opcode defaults
        // Opcodes before any header act as group-level defaults (common in
        // real files).
        SfzRegion* dest = &group_defaults;
        std::optional<SfzRegion> pending_region;
        std::string const base_dir = dir_of(path);

        auto finalize = [&]() {
            if (pending_region.has_value()) {
                finalize_region(res, std::move(*pending_region), base_dir);
                pending_region.reset();
            }
        };

        for (std::string const& tok : tokens) {
            if (!tok.empty() && tok.front() == '<') {
                finalize();
                if (is_header(tok, "region")) {
                    pending_region = group_defaults;  // inherit defaults
                    dest = &*pending_region;
                } else if (is_header(tok, "group") ||
                           is_header(tok, "global")) {
                    group_defaults = SfzRegion{};  // new inheritable context
                    dest = &group_defaults;
                } else {
                    // Unknown header (<control>, <curve>, <master>, ...):
                    // ignore it and its stray opcodes.
                    dest = nullptr;
                }
                continue;
            }
            std::size_t const eq = tok.find('=');
            if (eq == std::string::npos || eq == 0) {
                ++res.skipped_lines;  // garbage token
                continue;
            }
            if (dest == nullptr) {
                continue;  // opcode under an ignored header
            }
            std::string const key = ascii_lower(tok.substr(0, eq));
            std::string const value = tok.substr(eq + 1);
            if (!apply_opcode(*dest, key, value)) {
                // Unknown opcode: deliberately NOT an error, just ignored.
            }
        }
        finalize();
    } catch (std::exception const& e) {
        error = std::string{"sfz: parse failed (out of memory?): "} + e.what();
        return std::nullopt;
    }
    return res;
}

bool sampler_load_sfz(Sampler& sampler, std::string const& sfz_path,
                      std::string& error) {
    error.clear();
    std::optional<SfzParseResult> parsed = sfz_load(sfz_path, error);
    if (!parsed.has_value()) {
        return false;
    }
    if (parsed->regions.empty()) {
        error = "sfz: no usable regions in " + sfz_path +
                (parsed->skipped_regions > 0 ? " (all regions skipped)" : "");
        return false;
    }

    int loaded = 0;
    std::string last_error;
    try {
        for (SfzRegion const& r : parsed->regions) {
            // Zone note: pitch_keycenter, else key, else lo_key; transpose
            // shifts the effective mapping.
            int const center = r.pitch_keycenter >= 0 ? r.pitch_keycenter
                               : (r.key >= 0 ? r.key : r.lo_key);
            int const note = center + r.transpose;
            if (note < 0 || note > 127) {
                continue;
            }
            // One zone per MIDI note, first matching region wins.
            if (sampler.has_zone(note)) {
                continue;
            }
            std::optional<WavData> wav = wav_load(r.sample_path, last_error);
            if (!wav.has_value()) {
                continue;  // missing/broken sample: skip the region
            }
            if (r.volume_db != 0.0f) {
                // Fold per-region gain into the sample data (the Sampler
                // has no per-zone gain parameter).
                double const g =
                    std::pow(10.0, static_cast<double>(r.volume_db) / 20.0);
                for (float& s : wav->samples) {
                    s = static_cast<float>(static_cast<double>(s) * g);
                }
            }
            std::string zone_error;
            if (sampler.add_zone_data(note, *wav, zone_error)) {
                ++loaded;
            } else {
                last_error = zone_error;
            }
        }
    } catch (std::exception const& e) {
        error = std::string{"sfz: load failed: "} + e.what();
        return false;
    }

    if (loaded == 0) {
        error = "sfz: no zones loaded from " + sfz_path;
        if (!last_error.empty()) {
            error += " (last error: " + last_error + ")";
        }
        return false;
    }
    error.clear();
    return true;
}

}  // namespace addu94::audio
