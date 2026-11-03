// addu94 — tests/test_project.cpp
#include "test_framework.hpp"

#include "project/document.hpp"
#include "project/journal.hpp"

// See src/project/document.cpp: quiet the vendored nlohmann's use of the
// GCC 16-deprecated std::is_trivial.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using addu94::music::Clip;
using addu94::music::ClipEvent;
using addu94::project::Journal;
using addu94::project::JournalEntry;
using addu94::project::JournalOp;
using addu94::project::Project;
using addu94::project::SongSection;
using addu94::project::Track;

namespace {

std::atomic<int> temp_counter{0};

std::filesystem::path temp_dir(char const* tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("addu94_test_project_" + std::string(tag) + "_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              "_" + std::to_string(temp_counter.fetch_add(1)));
    std::filesystem::create_directories(p);
    return p;
}

bool events_equal(ClipEvent const& a, ClipEvent const& b) {
    return a.tick == b.tick && a.channel == b.channel && a.note == b.note &&
           a.velocity == b.velocity && a.on == b.on && a.take == b.take;
}

bool clips_equal(Clip const& a, Clip const& b) {
    if (a.length_ticks != b.length_ticks || a.next_take != b.next_take ||
        a.events.size() != b.events.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        if (!events_equal(a.events[i], b.events[i])) return false;
    }
    return true;
}

bool tracks_equal(Track const& a, Track const& b) {
    if (a.name != b.name || a.volume != b.volume || a.pan != b.pan ||
        a.mute != b.mute || a.solo != b.solo ||
        a.instrument != b.instrument || a.clips.size() != b.clips.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.clips.size(); ++i) {
        if (!clips_equal(a.clips[i], b.clips[i])) return false;
    }
    return true;
}

bool projects_equal(Project const& a, Project const& b) {
    if (a.schema != b.schema || a.name != b.name ||
        a.tempo_bpm != b.tempo_bpm || a.meter_num != b.meter_num ||
        a.meter_den != b.meter_den || a.key != b.key || a.scale != b.scale) {
        return false;
    }
    if (a.tracks.size() != b.tracks.size() ||
        a.sections.size() != b.sections.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!tracks_equal(a.tracks[i], b.tracks[i])) return false;
    }
    for (std::size_t i = 0; i < a.sections.size(); ++i) {
        auto const& x = a.sections[i];
        auto const& y = b.sections[i];
        if (x.name != y.name || x.start_bar != y.start_bar ||
            x.length_bars != y.length_bars) {
            return false;
        }
    }
    return true;
}

Project make_full_project() {
    Project p;
    p.name = "Full";
    p.tempo_bpm = 137.5;
    p.meter_num = 7;
    p.meter_den = 8;
    p.key = "Eb";
    p.scale = "minor";
    Track t;
    t.name = "Synth";
    t.volume = 1.25f;
    t.pan = -0.5f;
    t.mute = true;
    t.instrument = "synth";
    Clip c;
    c.length_ticks = 96 * 4 * 2;
    c.next_take = 2;
    c.events = {
        ClipEvent{0, 0, 60, 100, true, 0},
        ClipEvent{48, 1, 64, 90, false, 1},
        ClipEvent{95, 9, 127, 1, true, 2},
    };
    t.clips.push_back(std::move(c));
    p.tracks.push_back(std::move(t));
    p.sections.push_back(SongSection{"intro", 0, 4});
    p.sections.push_back(SongSection{"chorus", 4, 8});
    return p;
}

}  // namespace

TEST(project, roundtrip_default) {
    auto const p = Project::make_default();
    auto const s = addu94::project::project_to_json(p);
    Project q;
    std::string err;
    CHECK(addu94::project::project_from_json(s, q, err));
    CHECK(err.empty());
    CHECK(projects_equal(p, q));
}

TEST(project, roundtrip_full_with_clips_and_sections) {
    auto const p = make_full_project();
    auto const s = addu94::project::project_to_json(p);
    Project q;
    std::string err;
    CHECK(addu94::project::project_from_json(s, q, err));
    CHECK(err.empty());
    CHECK(projects_equal(p, q));
    // All serialized, including clip events.
    CHECK(s.find("\"events\":[{\"channel\":0,\"note\":60") != std::string::npos);
}

TEST(project, dump_is_deterministic_sorted) {
    auto const p = make_full_project();
    auto const s1 = addu94::project::project_to_json(p);
    auto const s2 = addu94::project::project_to_json(p);
    CHECK_EQ(s1, s2);
    // Sorted keys: "key" sorts before "meter_den" < ... < "tempo_bpm".
    CHECK(s1.starts_with("{\"key\":\"Eb\""));
}

TEST(project, malformed_never_throws) {
    Project out;
    std::string err;
    CHECK(!addu94::project::project_from_json("{not json!!", out, err));
    CHECK(!err.empty());
    CHECK(!addu94::project::project_from_json("{\"schema\":1,\"name\"", out,
                                              err));
    CHECK(!addu94::project::project_from_json("[]", out, err));
    CHECK(!addu94::project::project_from_json("null", out, err));
    CHECK(!addu94::project::project_from_json("42", out, err));
    CHECK(!addu94::project::project_from_json("", out, err));
    // Wrong types.
    CHECK(!addu94::project::project_from_json(R"({"schema":"1"})", out, err));
    CHECK(!addu94::project::project_from_json(R"({"schema":1,"name":7})", out,
                                              err));
    CHECK(!addu94::project::project_from_json(R"({"schema":1,"tracks":5})",
                                              out, err));
    CHECK(!addu94::project::project_from_json(
        R"({"schema":1,"tracks":[{"volume":"loud"}]})", out, err));
    CHECK(!addu94::project::project_from_json(
        R"({"schema":1,"tracks":[{"mute":"yes"}]})", out, err));
    // Missing fields.
    CHECK(!addu94::project::project_from_json(R"({"schema":1})", out, err));
    CHECK(!addu94::project::project_from_json(
        R"({"schema":1,"name":"a","tempo_bpm":120,"meter_num":4,)"
        R"("meter_den":4,"key":"C","tracks":[],"sections":[]})",
        out, err));  // missing "scale"
}

TEST(project, unsupported_schema_rejected) {
    Project out;
    std::string err;
    auto j = nlohmann::json::parse(
        addu94::project::project_to_json(make_full_project()));
    j["schema"] = 2;
    CHECK(!addu94::project::project_from_json(j.dump(), out, err));
    CHECK_EQ(err, "unsupported schema");
}

TEST(project, unknown_extra_fields_ignored) {
    auto const p = make_full_project();
    auto j = nlohmann::json::parse(addu94::project::project_to_json(p));
    j["something_new"] = nlohmann::json::array({"a", 1, true});
    j["tracks"][0]["future_param"] = 42;
    Project q;
    std::string err;
    CHECK(addu94::project::project_from_json(j.dump(), q, err));
    CHECK(projects_equal(p, q));
}

TEST(journal, append_and_replay) {
    auto dir = temp_dir("replay");
    std::string err;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(j.is_open());
        CHECK(j.entries().empty());
        CHECK_EQ(j.next_seq(), 1u);

        JournalEntry e;
        e.op = JournalOp::SetTempo;
        e.i = 13750;  // 137.50 bpm
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetMeter;
        e.i = 7 * 1000 + 8;
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetKey;
        e.s = "A";
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetScale;
        e.s = "dorian";
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::AddTrack;
        e.s = "Bass";
        CHECK(j.append(e));

        CHECK_EQ(j.entries().size(), 5u);
        CHECK_EQ(j.next_seq(), 6u);
    }
    auto p = Project::make_default();
    {
        Journal j;
        // Reopen: existing entries are restored (crash recovery).
        CHECK(j.open(dir.string(), err));
        CHECK_EQ(j.entries().size(), 5u);
        CHECK_EQ(j.next_seq(), 6u);
        CHECK(j.replay(p, err));
    }
    CHECK_NEAR(p.tempo_bpm, 137.5, 1e-9);
    CHECK_EQ(p.meter_num, 7);
    CHECK_EQ(p.meter_den, 8);
    CHECK_EQ(p.key, "A");
    CHECK_EQ(p.scale, "dorian");
    CHECK_EQ(p.tracks.size(), 5u);
    CHECK_EQ(p.tracks.back().name, "Bass");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, track_params_and_clip_ops) {
    auto dir = temp_dir("params");
    std::string err;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        JournalEntry e;
        e.op = JournalOp::SetTrackParam;
        e.i = 0;
        e.f = 1.25;
        e.s = "volume";
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetTrackParam;
        e.i = 0;
        e.f = -0.25;
        e.s = "pan";
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetTrackParam;
        e.i = 0;
        e.f = 1.0;
        e.s = "mute";
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::SetTrackParam;
        e.i = 0;
        e.s = "name=Lead";
        CHECK(j.append(e));
        // Record events: two into clip 0, one into clip 1 (slots created
        // on demand), then clear clip 1.
        nlohmann::json ev1 = {{"tick", 96}, {"channel", 0}, {"note", 60},
                              {"velocity", 100}, {"on", true}, {"take", 0}};
        nlohmann::json ev2 = {{"tick", 24}, {"channel", 1}, {"note", 64},
                              {"velocity", 90}, {"on", false}, {"take", 1}};
        e = JournalEntry{};
        e.op = JournalOp::ClipRecord;
        e.i = 0;
        e.f = 0.0;
        e.s = ev1.dump();
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::ClipRecord;
        e.i = 0;
        e.f = 0.0;
        e.s = ev2.dump();
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::ClipRecord;
        e.i = 0;
        e.f = 1.0;
        e.s = ev1.dump();
        CHECK(j.append(e));
        e = JournalEntry{};
        e.op = JournalOp::ClipClear;
        e.i = 0;
        e.f = 1.0;
        CHECK(j.append(e));
    }
    auto p = Project::make_default();
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK_EQ(j.entries().size(), 8u);
        CHECK(j.replay(p, err));
    }
    auto const& t = p.tracks[0];
    CHECK_NEAR(t.volume, 1.25, 1e-6);
    CHECK_NEAR(t.pan, -0.25, 1e-6);
    CHECK(t.mute);
    CHECK_EQ(t.name, "Lead");
    CHECK_EQ(t.clips.size(), 2u);
    CHECK_EQ(t.clips[0].events.size(), 2u);
    // insert_sorted: tick 24 before tick 96.
    CHECK_EQ(t.clips[0].events[0].tick, 24);
    CHECK_EQ(t.clips[0].events[0].note, 64);
    CHECK_EQ(t.clips[0].events[0].take, 1);
    CHECK_EQ(t.clips[0].events[1].tick, 96);
    CHECK_EQ(t.clips[0].events[1].take, 0);
    CHECK(t.clips[1].events.empty());  // cleared
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, replay_reports_unappliable_op) {
    auto dir = temp_dir("badop");
    std::string err;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        JournalEntry e;
        e.op = JournalOp::RemoveTrack;
        e.i = 9;  // only 4 default tracks
        CHECK(j.append(e));
    }
    auto p = Project::make_default();
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(!j.replay(p, err));
        CHECK(!err.empty());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, crash_truncated_tail_tolerated) {
    auto dir = temp_dir("crash");
    std::string err;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        for (int i = 0; i < 3; ++i) {
            JournalEntry e;
            e.op = JournalOp::AddTrack;
            e.s = "T" + std::to_string(i);
            CHECK(j.append(e));
        }
    }
    // Simulate a crash: raw bytes of a half-written line, no newline.
    {
        std::FILE* f =
            std::fopen((dir / "project.journal").string().c_str(), "ab");
        CHECK(f != nullptr);
        std::string const partial = "{\"seq\":4,\"ts\":1,\"op\":\"set_tem";
        std::fwrite(partial.data(), 1, partial.size(), f);
        std::fclose(f);
    }
    {
        Journal j;
        // Reopen: valid prefix is kept, truncated tail is dropped.
        CHECK(j.open(dir.string(), err));
        CHECK_EQ(j.entries().size(), 3u);
        CHECK_EQ(j.next_seq(), 4u);
        auto p = Project::make_default();
        CHECK(j.replay(p, err));
        CHECK_EQ(p.tracks.size(), 7u);  // 4 default + 3 journaled
    }
    // File is back to a clean, fully-parseable state.
    {
        std::ifstream f(dir / "project.journal");
        std::string line;
        int lines = 0;
        while (std::getline(f, line)) {
            ++lines;
            CHECK(line.ends_with("}"));
        }
        CHECK_EQ(lines, 3);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, checkpoint_replay_onto_default) {
    auto dir = temp_dir("checkpoint");
    std::string err;
    auto p = make_full_project();
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(j.checkpoint(p));
        CHECK_EQ(j.entries().size(), 1u);
        CHECK_EQ(j.entries()[0].op, JournalOp::Checkpoint);
    }
    Project q;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(j.replay(q, err));
    }
    CHECK(projects_equal(p, q));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, save_atomic_then_load) {
    auto dir = temp_dir("save");
    std::string err;
    auto p = make_full_project();
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        JournalEntry e;
        e.op = JournalOp::AddTrack;
        e.s = "Wip";  // un-checkpointed edit; folded away by the full save
        CHECK(j.append(e));
        CHECK(j.checkpoint(p));
        CHECK(j.save_atomic(p, dir.string(), err));
        // Journal truncated after save (rotated, fresh empty journal).
        CHECK_EQ(j.entries().size(), 0u);
        CHECK_EQ(j.next_seq(), 3u);  // seq counter survives the rotation
        CHECK_EQ(std::filesystem::file_size(dir / "project.journal"), 0u);
        CHECK(std::filesystem::exists(dir / "project.journal.1"));
        CHECK(!std::filesystem::exists(dir / "project.json.tmp"));
    }
    Project q;
    CHECK(addu94::project::load_project(dir.string(), q, err));
    CHECK(err.empty());
    CHECK(projects_equal(p, q));
    // A fresh journal in the same dir has nothing left to replay.
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(j.entries().empty());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, load_empty_dir_gives_default) {
    auto dir = temp_dir("empty");
    std::string err;
    Project q;
    CHECK(addu94::project::load_project(dir.string(), q, err));
    CHECK(projects_equal(q, Project::make_default()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, load_quarantines_corrupt_project_json) {
    auto dir = temp_dir("corrupt");
    {
        std::ofstream f(dir / "project.json");
        f << "{\"schema\":1,\"nam";  // truncated garbage
    }
    std::string err;
    Project q;
    CHECK(addu94::project::load_project(dir.string(), q, err));
    CHECK(projects_equal(q, Project::make_default()));
    // Corrupt file quarantined, not deleted.
    bool found = false;
    for (auto const& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().string().find(".corrupt-") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
    CHECK(!std::filesystem::exists(dir / "project.json"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, load_recovers_project_plus_journal) {
    auto dir = temp_dir("load");
    std::string err;
    auto p = make_full_project();
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        CHECK(j.checkpoint(p));
        // Two live edits on top of the checkpoint (applied to p too, as
        // the app would).
        JournalEntry e;
        e.op = JournalOp::SetTempo;
        e.i = 9600;  // 96.00 bpm
        CHECK(j.append(e));
        p.tempo_bpm = 96.0;
        e = JournalEntry{};
        e.op = JournalOp::AddTrack;
        e.s = "Live";
        CHECK(j.append(e));
        Track live;
        live.name = "Live";
        p.tracks.push_back(std::move(live));  // app applies the same edit
    }
    Project q;
    CHECK(addu94::project::load_project(dir.string(), q, err));
    CHECK(projects_equal(p, q));  // checkpoint + 2 journal edits
    CHECK_EQ(q.tracks.back().name, "Live");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(journal, at_most_one_second_of_lost_edits) {
    // 10 rapid appends, no checkpoint: every one must come back.
    auto dir = temp_dir("loss");
    std::string err;
    {
        Journal j;
        CHECK(j.open(dir.string(), err));
        for (int i = 0; i < 10; ++i) {
            JournalEntry e;
            e.op = JournalOp::AddTrack;
            e.s = "R" + std::to_string(i);
            CHECK(j.append(e));
        }
    }
    Project q;
    CHECK(addu94::project::load_project(dir.string(), q, err));
    CHECK_EQ(q.tracks.size(), 14u);  // 4 default + 10 recovered
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(q.tracks[static_cast<std::size_t>(4 + i)].name,
                 "R" + std::to_string(i));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_MAIN()
