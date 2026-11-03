// addu94 — src/project/journal.cpp
#include "project/journal.hpp"

// See document.cpp: vendored nlohmann uses std::is_trivial (deprecated in
// GCC 16); third_party is not SYSTEM in the manual verification command.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace addu94::project {

namespace {

using nlohmann::json;

char const* op_name(JournalOp op) noexcept {
    switch (op) {
    case JournalOp::Checkpoint: return "checkpoint";
    case JournalOp::SetTempo: return "set_tempo";
    case JournalOp::SetMeter: return "set_meter";
    case JournalOp::SetKey: return "set_key";
    case JournalOp::SetScale: return "set_scale";
    case JournalOp::AddTrack: return "add_track";
    case JournalOp::RemoveTrack: return "remove_track";
    case JournalOp::SetTrackParam: return "set_track_param";
    case JournalOp::ClipRecord: return "clip_record";
    case JournalOp::ClipClear: return "clip_clear";
    }
    return "?";
}

[[nodiscard]] bool op_from_name(std::string_view name, JournalOp& out) noexcept {
    if (name == "checkpoint") { out = JournalOp::Checkpoint; return true; }
    if (name == "set_tempo") { out = JournalOp::SetTempo; return true; }
    if (name == "set_meter") { out = JournalOp::SetMeter; return true; }
    if (name == "set_key") { out = JournalOp::SetKey; return true; }
    if (name == "set_scale") { out = JournalOp::SetScale; return true; }
    if (name == "add_track") { out = JournalOp::AddTrack; return true; }
    if (name == "remove_track") { out = JournalOp::RemoveTrack; return true; }
    if (name == "set_track_param") { out = JournalOp::SetTrackParam; return true; }
    if (name == "clip_record") { out = JournalOp::ClipRecord; return true; }
    if (name == "clip_clear") { out = JournalOp::ClipClear; return true; }
    return false;
}

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] std::string journal_path(std::string const& dir) {
    return (std::filesystem::path(dir) / "project.journal").string();
}

// Serialize one entry to a line (sorted keys, '\n' terminated). Never
// throws; fails only on pathological strings (invalid UTF-8 would have to
// come from program memory).
[[nodiscard]] bool serialize_entry(JournalEntry const& e, std::string& line) {
    try {
        json j;
        j["seq"] = e.seq;
        j["ts"] = e.ts_ns;
        j["op"] = op_name(e.op);
        j["i"] = e.i;
        j["f"] = e.f;
        j["s"] = e.s;
        line = j.dump();
        line.push_back('\n');
        return true;
    } catch (...) {
        return false;
    }
}

// Parse one journal line. Tolerant of missing optional fields (defaults),
// strict about op names and types.
[[nodiscard]] bool parse_entry(std::string_view line, JournalEntry& e,
                               std::string& error) {
    json const j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "journal: malformed line";
        return false;
    }
    JournalEntry out;
    auto const op_it = j.find("op");
    if (op_it == j.end() || !op_it->is_string()) {
        error = "journal: missing 'op'";
        return false;
    }
    if (!op_from_name(op_it->get<std::string>(), out.op)) {
        error = "journal: unknown op";
        return false;
    }
    if (auto it = j.find("seq"); it != j.end() && it->is_number_integer()) {
        out.seq = it->get<std::uint64_t>();
    }
    if (auto it = j.find("ts"); it != j.end() && it->is_number_integer()) {
        out.ts_ns = it->get<std::uint64_t>();
    }
    if (auto it = j.find("i"); it != j.end() && it->is_number_integer()) {
        out.i = it->get<std::int64_t>();
    }
    if (auto it = j.find("f"); it != j.end() && it->is_number()) {
        out.f = it->get<double>();
    }
    if (auto it = j.find("s"); it != j.end() && it->is_string()) {
        out.s = it->get<std::string>();
    }
    e = std::move(out);
    return true;
}

// --- strict field extractors (mirror document.cpp; kept local so the JSON
// --- helpers stay file-local) -------------------------------------------

[[nodiscard]] bool j_get_i64(json const& obj, char const* key,
                             std::int64_t& out, std::string& error) {
    auto const it = obj.find(key);
    if (it == obj.end()) {
        error = std::string("missing field '") + key + "'";
        return false;
    }
    if (!it->is_number_integer()) {
        error = std::string("field '") + key + "' is not an integer";
        return false;
    }
    out = it->get<std::int64_t>();
    return true;
}

[[nodiscard]] bool j_get_int(json const& obj, char const* key, int& out,
                             std::string& error) {
    std::int64_t v = 0;
    if (!j_get_i64(obj, key, v, error)) return false;
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) {
        error = std::string("field '") + key + "' out of int range";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

[[nodiscard]] bool j_get_u8(json const& obj, char const* key,
                            std::uint8_t& out, std::string& error) {
    std::int64_t v = 0;
    if (!j_get_i64(obj, key, v, error)) return false;
    if (v < 0 || v > 255) {
        error = std::string("field '") + key + "' out of byte range";
        return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

[[nodiscard]] bool j_get_bool(json const& obj, char const* key, bool& out,
                              std::string& error) {
    auto const it = obj.find(key);
    if (it == obj.end()) {
        error = std::string("missing field '") + key + "'";
        return false;
    }
    if (!it->is_boolean()) {
        error = std::string("field '") + key + "' is not a boolean";
        return false;
    }
    out = it->get<bool>();
    return true;
}

// Apply one entry to the project. All ops are forward-only; out-of-range
// indices are errors (a journal is program-written data, so this means
// corruption) except ClipClear on an absent clip slot, which is a no-op.
[[nodiscard]] bool apply_entry(Project& p, JournalEntry const& e,
                               std::string& error) {
    switch (e.op) {
    case JournalOp::Checkpoint: {
        Project loaded;
        std::string perr;
        if (!project_from_json(e.s, loaded, perr)) {
            error = "checkpoint payload invalid: " + perr;
            return false;
        }
        p = std::move(loaded);
        return true;
    }
    case JournalOp::SetTempo:
        p.tempo_bpm = static_cast<double>(e.i) / 100.0;
        return true;
    case JournalOp::SetMeter: {
        if (e.i < 0) {
            error = "set_meter: negative value";
            return false;
        }
        p.meter_num = static_cast<int>(e.i / 1000);
        p.meter_den = static_cast<int>(e.i % 1000);
        if (p.meter_den <= 0) {
            error = "set_meter: invalid denominator";
            return false;
        }
        return true;
    }
    case JournalOp::SetKey:
        p.key = e.s;
        return true;
    case JournalOp::SetScale:
        p.scale = e.s;
        return true;
    case JournalOp::AddTrack: {
        Track t;
        t.name = e.s;
        p.tracks.push_back(std::move(t));
        return true;
    }
    case JournalOp::RemoveTrack: {
        if (e.i < 0 || static_cast<std::size_t>(e.i) >= p.tracks.size()) {
            error = "remove_track: index out of range";
            return false;
        }
        p.tracks.erase(p.tracks.begin() + static_cast<std::ptrdiff_t>(e.i));
        return true;
    }
    case JournalOp::SetTrackParam: {
        if (e.i < 0 || static_cast<std::size_t>(e.i) >= p.tracks.size()) {
            error = "set_track_param: track index out of range";
            return false;
        }
        Track& t = p.tracks[static_cast<std::size_t>(e.i)];
        // Numeric/bool params carry the value in f; string params encode
        // "param=value" in s (an entry has only one string slot).
        auto const eq = e.s.find('=');
        std::string const param =
            (eq == std::string::npos) ? e.s : e.s.substr(0, eq);
        std::string const value =
            (eq == std::string::npos) ? std::string() : e.s.substr(eq + 1);
        if (param == "volume") {
            t.volume = static_cast<float>(e.f);
        } else if (param == "pan") {
            t.pan = static_cast<float>(e.f);
        } else if (param == "mute") {
            t.mute = (e.f != 0.0);
        } else if (param == "solo") {
            t.solo = (e.f != 0.0);
        } else if (param == "name" && eq != std::string::npos) {
            t.name = value;
        } else if (param == "instrument" && eq != std::string::npos) {
            t.instrument = value;
        } else {
            error = "set_track_param: bad param '" + e.s + "'";
            return false;
        }
        return true;
    }
    case JournalOp::ClipRecord: {
        if (e.i < 0 || static_cast<std::size_t>(e.i) >= p.tracks.size()) {
            error = "clip_record: track index out of range";
            return false;
        }
        Track& t = p.tracks[static_cast<std::size_t>(e.i)];
        if (e.f < 0.0 || e.f > 1e9) {
            error = "clip_record: clip index out of range";
            return false;
        }
        auto const clip_idx = static_cast<std::size_t>(e.f);
        if (clip_idx >= t.clips.size()) {
            t.clips.resize(clip_idx + 1);  // create slot on demand
        }
        json const j = json::parse(e.s, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            error = "clip_record: malformed event JSON";
            return false;
        }
        music::ClipEvent ev;
        if (!j_get_i64(j, "tick", ev.tick, error)) return false;
        if (!j_get_u8(j, "channel", ev.channel, error)) return false;
        if (!j_get_u8(j, "note", ev.note, error)) return false;
        if (!j_get_u8(j, "velocity", ev.velocity, error)) return false;
        if (!j_get_bool(j, "on", ev.on, error)) return false;
        if (!j_get_int(j, "take", ev.take, error)) return false;
        t.clips[clip_idx].insert_sorted(ev);
        return true;
    }
    case JournalOp::ClipClear: {
        if (e.i < 0 || static_cast<std::size_t>(e.i) >= p.tracks.size()) {
            error = "clip_clear: track index out of range";
            return false;
        }
        Track& t = p.tracks[static_cast<std::size_t>(e.i)];
        if (e.f >= 0.0 && e.f <= 1e9) {
            auto const clip_idx = static_cast<std::size_t>(e.f);
            if (clip_idx < t.clips.size()) t.clips[clip_idx].clear();
        }
        return true;
    }
    }
    error = "journal: unknown op";
    return false;
}

void fsync_dir(std::filesystem::path const& dir) noexcept {
    int const fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

Journal::~Journal() { close(); }

void Journal::close() noexcept {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    dir_.clear();
    entries_.clear();
    next_seq_ = 1;
}

bool Journal::is_open() const noexcept { return file_ != nullptr; }

bool Journal::attach_file(std::string const& path, std::string const& dir,
                          std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (f == nullptr) {
        error = "journal: cannot open '" + path + "'";
        return false;
    }
    file_ = f;
    dir_ = dir;
    return true;
}

bool Journal::open(std::string const& dir, std::string& error) {
    close();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string const path = journal_path(dir);

    // Read existing entries to restore the seq counter. A corrupt tail
    // (truncated last line, or a malformed line) ends the valid prefix;
    // the file is truncated back to it so future appends stay parseable.
    std::string contents;
    {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) {
            contents.assign(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>());
        }
    }
    std::size_t pos = 0;
    std::size_t good_end = 0;
    bool corrupt = false;
    while (pos < contents.size()) {
        auto const nl = contents.find('\n', pos);
        if (nl == std::string::npos) {  // truncated last line
            corrupt = true;
            break;
        }
        JournalEntry e;
        std::string lerr;
        if (!parse_entry(std::string_view(contents).substr(pos, nl - pos), e,
                         lerr)) {
            corrupt = true;
            break;
        }
        entries_.push_back(std::move(e));
        pos = nl + 1;
        good_end = pos;
    }
    if (corrupt && good_end < contents.size()) {
        if (::truncate(path.c_str(), static_cast<off_t>(good_end)) != 0) {
            error = "journal: cannot truncate corrupt tail of '" + path + "'";
            return false;
        }
    }
    next_seq_ = entries_.empty() ? 1 : entries_.back().seq + 1;
    return attach_file(path, dir, error);
}

bool Journal::append(JournalEntry const& e) {
    if (file_ == nullptr) return false;
    JournalEntry out = e;
    if (out.seq == 0) out.seq = next_seq_;
    if (out.ts_ns == 0) out.ts_ns = now_ns();
    std::string line;
    if (!serialize_entry(out, line)) return false;
    std::size_t const n = std::fwrite(line.data(), 1, line.size(), file_);
    if (n != line.size()) return false;
    if (std::fflush(file_) != 0) return false;
    // Per-append fsync: the "≤ 1 s lost edits" guarantee needs each edit
    // durable before it is acknowledged.
    if (::fsync(::fileno(file_)) != 0) return false;
    if (out.seq >= next_seq_) next_seq_ = out.seq + 1;
    entries_.push_back(std::move(out));
    return true;
}

bool Journal::replay(Project& p, std::string& error) const {
    for (auto const& e : entries_) {
        if (!apply_entry(p, e, error)) return false;
    }
    return true;
}

bool Journal::checkpoint(Project const& p) {
    JournalEntry e;
    e.op = JournalOp::Checkpoint;
    e.s = project_to_json(p);
    if (e.s.empty()) return false;
    return append(e);
}

bool Journal::save_atomic(Project const& p, std::string const& dir,
                          std::string& error) {
    std::string const text = project_to_json(p);
    if (text.empty()) {
        error = "save_atomic: project_to_json failed";
        return false;
    }
    std::filesystem::path const dirp(dir);
    std::string const final_path = (dirp / "project.json").string();
    std::string const tmp_path = final_path + ".tmp";

    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "wb");
        if (f == nullptr) {
            error = "save_atomic: cannot create '" + tmp_path + "'";
            return false;
        }
        bool ok = true;
        std::size_t const n = std::fwrite(text.data(), 1, text.size(), f);
        if (n != text.size()) ok = false;
        if (ok && std::fflush(f) != 0) ok = false;
        if (ok && ::fsync(::fileno(f)) != 0) ok = false;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) {
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            error = "save_atomic: writing '" + tmp_path + "' failed";
            return false;
        }
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        error = "save_atomic: rename '" + tmp_path + "' failed";
        return false;
    }
    fsync_dir(dirp);  // make the rename itself durable

    // Rotate the journal: project.journal → project.journal.1 (the rename
    // overwrites the previous generation; nothing is deleted), then start
    // a fresh empty journal. Safe because project.json is durable now.
    std::string const path = journal_path(dir);
    bool const owns_dir = is_open() && dir_ == dirp.string();
    if (owns_dir) {
        std::fclose(file_);
        file_ = nullptr;
    }
    std::error_code ec;
    std::filesystem::rename(path, path + ".1", ec);
    // Missing journal (fresh dir) is fine; rename overwrites the old .1.
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) {
            error = "save_atomic: cannot create fresh '" + path + "'";
            return false;
        }
        std::fclose(f);
    }
    fsync_dir(dirp);
    if (owns_dir) {
        entries_.clear();
        if (!attach_file(path, dir, error)) return false;
    }
    return true;
}

std::vector<JournalEntry> const& Journal::entries() const noexcept {
    return entries_;
}

std::uint64_t Journal::next_seq() const noexcept { return next_seq_; }

bool load_project(std::string const& dir, Project& out, std::string& error) {
    out = Project::make_default();
    std::filesystem::path const dirp(dir);
    std::string const json_path = (dirp / "project.json").string();

    std::error_code ec;
    if (std::filesystem::exists(json_path, ec)) {
        std::string contents;
        {
            std::ifstream f(json_path, std::ios::binary);
            if (f.good()) {
                contents.assign(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
            }
        }
        Project loaded;
        std::string perr;
        if (!contents.empty() && project_from_json(contents, loaded, perr)) {
            out = std::move(loaded);
        } else {
            // Quarantine the corrupt file (never delete silently) and fall
            // back to the default project; the journal replays on top.
            auto const ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count();
            std::string const quarantine =
                json_path + ".corrupt-" + std::to_string(ts);
            std::error_code rn_ec;
            std::filesystem::rename(json_path, quarantine, rn_ec);
        }
    }

    std::string const path = journal_path(dir);
    if (std::filesystem::exists(path, ec)) {
        Journal j;
        if (!j.open(dir, error)) return false;
        if (!j.replay(out, error)) return false;
    }
    return true;
}

}  // namespace addu94::project
