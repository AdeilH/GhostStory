// ghost_story — src/project/document.cpp
#include "project/document.hpp"

// The vendored nlohmann (3.11.3) uses std::is_trivial in a static_assert,
// which GCC 16 deprecates. CMake adds third_party as SYSTEM (warnings off);
// the plain -I verification command does not, so quiet it here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

#include <limits>
#include <string>

namespace ghost_story::project {

namespace {

using nlohmann::json;

// --- strict field extractors -------------------------------------------
// Malformed input never throws: every accessor type-checks and reports
// through `error`. Unknown extra fields are ignored (forward migration).

[[nodiscard]] bool get_i64(json const& obj, char const* key,
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

[[nodiscard]] bool get_int(json const& obj, char const* key, int& out,
                           std::string& error) {
    std::int64_t v = 0;
    if (!get_i64(obj, key, v, error)) return false;
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) {
        error = std::string("field '") + key + "' out of int range";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

[[nodiscard]] bool get_u8(json const& obj, char const* key, std::uint8_t& out,
                          std::string& error) {
    std::int64_t v = 0;
    if (!get_i64(obj, key, v, error)) return false;
    if (v < 0 || v > 255) {
        error = std::string("field '") + key + "' out of byte range";
        return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

[[nodiscard]] bool get_f64(json const& obj, char const* key, double& out,
                           std::string& error) {
    auto const it = obj.find(key);
    if (it == obj.end()) {
        error = std::string("missing field '") + key + "'";
        return false;
    }
    if (!it->is_number()) {  // integer or float both round-trip fine
        error = std::string("field '") + key + "' is not a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

[[nodiscard]] bool get_bool(json const& obj, char const* key, bool& out,
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

[[nodiscard]] bool get_str(json const& obj, char const* key, std::string& out,
                           std::string& error) {
    auto const it = obj.find(key);
    if (it == obj.end()) {
        error = std::string("missing field '") + key + "'";
        return false;
    }
    if (!it->is_string()) {
        error = std::string("field '") + key + "' is not a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

[[nodiscard]] bool expect_array(json const& obj, char const* key,
                                json const*& out, std::string& error) {
    auto const it = obj.find(key);
    if (it == obj.end()) {
        error = std::string("missing field '") + key + "'";
        return false;
    }
    if (!it->is_array()) {
        error = std::string("field '") + key + "' is not an array";
        return false;
    }
    out = &(*it);
    return true;
}

// --- serialization ------------------------------------------------------

[[nodiscard]] json clip_event_to_json(music::ClipEvent const& e) {
    json j;
    j["tick"] = e.tick;
    j["channel"] = e.channel;
    j["note"] = e.note;
    j["velocity"] = e.velocity;
    j["on"] = e.on;
    j["take"] = e.take;
    return j;
}

[[nodiscard]] bool clip_from_json(json const& j, music::Clip& c,
                                  std::string& error) {
    if (!j.is_object()) {
        error = "clip: expected object";
        return false;
    }
    if (!get_i64(j, "length_ticks", c.length_ticks, error)) return false;
    // Optional placement — older project files predate clip placement.
    c.start_tick = j.value("start_tick", std::int64_t{0});
    if (!get_int(j, "next_take", c.next_take, error)) return false;
    json const* events = nullptr;
    if (!expect_array(j, "events", events, error)) return false;
    c.events.reserve(events->size());
    for (auto const& je : *events) {
        if (!je.is_object()) {
            error = "clip event: expected object";
            return false;
        }
        music::ClipEvent e;
        if (!get_i64(je, "tick", e.tick, error)) return false;
        if (!get_u8(je, "channel", e.channel, error)) return false;
        if (!get_u8(je, "note", e.note, error)) return false;
        if (!get_u8(je, "velocity", e.velocity, error)) return false;
        if (!get_bool(je, "on", e.on, error)) return false;
        if (!get_int(je, "take", e.take, error)) return false;
        c.events.push_back(e);
    }
    // Optional take boxes — the per-take blocks the arrangement draws.
    // Files from before the boxes existed simply omit the field (the clip
    // then renders as one whole-window block, the old honest view).
    if (auto const it = j.find("boxes"); it != j.end() && it->is_array()) {
        for (auto const& jb : *it) {
            music::TakeBox b;
            if (!get_int(jb, "take", b.take, error)) return false;
            if (!get_i64(jb, "start_tick", b.start_tick, error)) return false;
            if (!get_i64(jb, "length_ticks", b.length_ticks, error)) {
                return false;
            }
            c.boxes.push_back(b);
        }
    }
    return true;
}

[[nodiscard]] bool track_from_json(json const& j, Track& t,
                                   std::string& error) {
    if (!j.is_object()) {
        error = "track: expected object";
        return false;
    }
    if (!get_str(j, "name", t.name, error)) return false;
    double volume = 0.0;
    double pan = 0.0;
    if (!get_f64(j, "volume", volume, error)) return false;
    if (!get_f64(j, "pan", pan, error)) return false;
    t.volume = static_cast<float>(volume);
    t.pan = static_cast<float>(pan);
    if (!get_bool(j, "mute", t.mute, error)) return false;
    if (!get_bool(j, "solo", t.solo, error)) return false;
    if (!get_str(j, "instrument", t.instrument, error)) return false;
    json const* clips = nullptr;
    if (!expect_array(j, "clips", clips, error)) return false;
    t.clips.reserve(clips->size());
    for (auto const& jc : *clips) {
        music::Clip c;
        if (!clip_from_json(jc, c, error)) return false;
        t.clips.push_back(std::move(c));
    }
    return true;
}

}  // namespace

std::string project_to_json(Project const& p) {
    try {
        json j;
        j["schema"] = p.schema;
        j["name"] = p.name;
        j["tempo_bpm"] = p.tempo_bpm;
        j["meter_num"] = p.meter_num;
        j["meter_den"] = p.meter_den;
        j["key"] = p.key;
        j["scale"] = p.scale;

        json tracks = json::array();
        tracks.get_ref<json::array_t&>().reserve(p.tracks.size());
        for (auto const& t : p.tracks) {
            json jt;
            jt["name"] = t.name;
            jt["volume"] = static_cast<double>(t.volume);
            jt["pan"] = static_cast<double>(t.pan);
            jt["mute"] = t.mute;
            jt["solo"] = t.solo;
            jt["instrument"] = t.instrument;
            json clips = json::array();
            clips.get_ref<json::array_t&>().reserve(t.clips.size());
            for (auto const& c : t.clips) {
                json jc;
                jc["length_ticks"] = c.length_ticks;
                jc["start_tick"] = c.start_tick;  // timeline placement
                jc["next_take"] = c.next_take;
                json events = json::array();
                events.get_ref<json::array_t&>().reserve(c.events.size());
                for (auto const& e : c.events) {
                    events.push_back(clip_event_to_json(e));
                }
                jc["events"] = std::move(events);
                json boxes = json::array();
                boxes.get_ref<json::array_t&>().reserve(c.boxes.size());
                for (auto const& b : c.boxes) {
                    boxes.push_back(json{{"take", b.take},
                                         {"start_tick", b.start_tick},
                                         {"length_ticks", b.length_ticks}});
                }
                jc["boxes"] = std::move(boxes);
                clips.push_back(std::move(jc));
            }
            jt["clips"] = std::move(clips);
            tracks.push_back(std::move(jt));
        }
        j["tracks"] = std::move(tracks);

        json sections = json::array();
        sections.get_ref<json::array_t&>().reserve(p.sections.size());
        for (auto const& s : p.sections) {
            json js;
            js["name"] = s.name;
            js["start_bar"] = s.start_bar;
            js["length_bars"] = s.length_bars;
            sections.push_back(std::move(js));
        }
        j["sections"] = std::move(sections);

        // Default (sorted-key) dump → deterministic bytes for a given state.
        return j.dump();
    } catch (...) {
        // dump() can only throw on invalid UTF-8 that would have to come
        // from program memory; degrade to empty (callers treat as failure)
        // rather than letting an exception escape.
        return {};
    }
}

bool project_from_json(std::string const& json_str, Project& out,
                       std::string& error) {
    error.clear();
    json const j = json::parse(json_str, nullptr, false);
    if (j.is_discarded()) {
        error = "invalid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "project: expected object";
        return false;
    }

    std::int64_t schema = 0;
    if (!get_i64(j, "schema", schema, error)) return false;
    if (schema != 1) {
        error = "unsupported schema";
        return false;
    }

    Project p;
    if (!get_str(j, "name", p.name, error)) return false;
    if (!get_f64(j, "tempo_bpm", p.tempo_bpm, error)) return false;
    if (!get_int(j, "meter_num", p.meter_num, error)) return false;
    if (!get_int(j, "meter_den", p.meter_den, error)) return false;
    if (!get_str(j, "key", p.key, error)) return false;
    if (!get_str(j, "scale", p.scale, error)) return false;

    json const* tracks = nullptr;
    if (!expect_array(j, "tracks", tracks, error)) return false;
    p.tracks.reserve(tracks->size());
    for (auto const& jt : *tracks) {
        Track t;
        if (!track_from_json(jt, t, error)) return false;
        p.tracks.push_back(std::move(t));
    }

    json const* sections = nullptr;
    if (!expect_array(j, "sections", sections, error)) return false;
    p.sections.reserve(sections->size());
    for (auto const& js : *sections) {
        if (!js.is_object()) {
            error = "section: expected object";
            return false;
        }
        SongSection s;
        if (!get_str(js, "name", s.name, error)) return false;
        if (!get_int(js, "start_bar", s.start_bar, error)) return false;
        if (!get_int(js, "length_bars", s.length_bars, error)) return false;
        p.sections.push_back(std::move(s));
    }

    out = std::move(p);
    return true;
}

}  // namespace ghost_story::project
