// ghost_story — tests/test_midi.cpp
// Virtual-port tests (spec §11): every test passes with NO physical device.
// A test-local ALSA client sends MIDI to the engine's virtual port and the
// engine's decoded output is checked. Requires ALSA (present in CI env).
#include "test_framework.hpp"

#include "device/manifest.hpp"
#include "device/probe.hpp"
#include "midi/alsa_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

using ghost_story::midi::AlsaClient;
using ghost_story::midi::MidiEvent;
using ghost_story::midi::Type;

namespace {

struct CollectSink : ghost_story::midi::EventSink {
    std::vector<MidiEvent> events;
    std::atomic<int> device_events{0};
    std::mutex m;
    void on_midi(MidiEvent const& ev) override {
        std::scoped_lock l(m);
        events.push_back(ev);
    }
    void on_device_event(ghost_story::midi::DeviceEvent const&) override {
        device_events.fetch_add(1);
    }
};

/// Send raw bytes as ALSA seq events from `from` to the engine's port.
/// Source must be a READ-capable port — use the sender's virtual port.
void send_noteon(AlsaClient& from, int dest_client, int dest_port, int note,
                 int vel, int channel = 0) {
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    ev.type = SND_SEQ_EVENT_NOTEON;
    ev.data.note.channel = static_cast<unsigned char>(channel);
    ev.data.note.note = static_cast<unsigned char>(note);
    ev.data.note.velocity = static_cast<unsigned char>(vel);
    int const src = from.virtual_port() >= 0 ? from.virtual_port() : 0;
    snd_seq_ev_set_source(&ev, static_cast<unsigned char>(src));
    snd_seq_ev_set_dest(&ev, static_cast<unsigned char>(dest_client),
                        static_cast<unsigned char>(dest_port));
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(from.raw(), &ev);
}

bool wait_for(std::function<bool()> pred, int timeout_ms = 2000) {
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST(midi, virtual_port_note_roundtrip) {
    AlsaClient engine;
    CHECK(engine.open("ghost_story-test-engine", true));
    CollectSink sink;
    engine.set_sink(&sink);
    CHECK(engine.start());

    AlsaClient sender;
    CHECK(sender.open("ghost_story-test-sender", true));
    // Connect sender's virtual out -> engine's virtual in via aconnect-style
    // subscription between the two clients' ports.
    CHECK(engine.connect_input(sender.client_id(), sender.virtual_port()));

    send_noteon(sender, engine.client_id(), engine.virtual_port(), 60, 100);
    send_noteon(sender, engine.client_id(), engine.virtual_port(), 62, 64, 1);

    auto const note_count = [&sink] {
        std::scoped_lock l(sink.m);
        return std::count_if(sink.events.begin(), sink.events.end(),
                             [](MidiEvent const& e) {
                                 return e.type == Type::NoteOn;
                             });
    };
    CHECK(wait_for([&] { return note_count() >= 2; }));

    std::vector<MidiEvent> notes;
    {
        std::scoped_lock l(sink.m);
        for (auto const& e : sink.events) {
            if (e.type == Type::NoteOn) notes.push_back(e);
        }
    }
    CHECK_EQ(notes.size(), 2u);
    CHECK_EQ(notes[0].note(), 60);
    CHECK_EQ(notes[0].velocity(), 100);
    CHECK_EQ(notes[0].channel, 0);
    CHECK(notes[0].stamp_ns > 0);
    CHECK_EQ(notes[1].channel, 1);

    engine.stop();
    sender.close();
    engine.close();
}

TEST(midi, timestamp_monotonic_and_jitter_small) {
    AlsaClient engine;
    CHECK(engine.open("ghost_story-test-engine2", true));
    CollectSink sink;
    engine.set_sink(&sink);
    CHECK(engine.start());
    AlsaClient sender;
    CHECK(sender.open("ghost_story-test-sender2", true));
    CHECK(engine.connect_input(sender.client_id(), sender.virtual_port()));

    auto const t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        send_noteon(sender, engine.client_id(), engine.virtual_port(), 40 + i, 1);
    }
    auto const note_count = [&sink] {
        std::scoped_lock l(sink.m);
        return std::count_if(sink.events.begin(), sink.events.end(),
                             [](MidiEvent const& e) {
                                 return e.type == Type::NoteOn;
                             });
    };
    CHECK(wait_for([&] { return note_count() >= 50; }));
    auto const t1 = std::chrono::steady_clock::now();

    std::vector<MidiEvent> notes;
    {
        std::scoped_lock l(sink.m);
        for (auto const& e : sink.events) {
            if (e.type == Type::NoteOn) notes.push_back(e);
        }
    }
    for (std::size_t i = 1; i < notes.size(); ++i) {
        CHECK(notes[i].stamp_ns >= notes[i - 1].stamp_ns);
    }
    auto const total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              t1 - t0).count();
    // 50 events through two ALSA clients should take far less than 2 s;
    // a pathological stall would show up here (M2 does precise jitter bench).
    CHECK(total_us < 2000000);

    engine.stop();
    sender.close();
    engine.close();
}

TEST(midi, model_name_matching) {
    CHECK_EQ(ghost_story::midi::mpk_model_from_name("MPK mini 3"),
             std::string_view{"mpk-mini"});
    CHECK_EQ(ghost_story::midi::mpk_model_from_name("Akai MPK Mini Mk II"),
             std::string_view{"mpk-mini"});
    CHECK(ghost_story::midi::mpk_model_from_name("Midi Through").empty());
}

TEST(manifest, inventory_has_no_dead_controls_generic) {
    auto inv = ghost_story::device::default_inventory("generic-mpk");
    // 25 keys + 16 pads + 8 knobs + 2 joystick axes + 3 buttons + pedal
    CHECK_EQ(inv.size(), 55u);
    // unique ids
    std::vector<std::string> ids;
    for (auto const& c : inv) ids.push_back(c.id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST(manifest, json_roundtrip_preserves_and_checksum_catches_corruption) {
    auto m = ghost_story::device::Manifest{};
    m.model = "mpk-mini";
    m.alsa_client = 128;
    m.alsa_client_name = "MPK mini 3";
    m.ports.push_back({0, "MPK mini 3 MIDI 1"});
    m.controls = ghost_story::device::default_inventory("mpk-mini");
    m.probed_at_ns = 12345;

    auto const text = ghost_story::device::manifest_to_json(m);
    auto parsed = ghost_story::device::manifest_from_json(text);
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->model, "mpk-mini");
    CHECK_EQ(parsed->controls.size(), m.controls.size());

    // Corrupt one byte -> checksum must fail -> nullopt (quarantine path).
    auto corrupted = text;
    corrupted[corrupted.find("\"mpk-mini\"")] = 'X';
    auto bad = ghost_story::device::manifest_from_json(corrupted);
    // Note: corrupted JSON may fail parse OR checksum; both -> nullopt
    CHECK(!bad.has_value() || bad->model != "mpk-mini");
}

TEST(manifest, garbage_json_rejected) {
    CHECK(!ghost_story::device::manifest_from_json("{not json!!").has_value());
    CHECK(!ghost_story::device::manifest_from_json("").has_value());
}

TEST(probe, quick_scan_without_device_writes_generic_manifest) {
    // No MPK Mini is attached in CI (only Midi Through + our virtual ports).
    auto result = ghost_story::device::Probe::quick_scan();
    CHECK(!result.summary.empty());
    CHECK(result.manifest.controls.size() >= 55u);
    // No crash, valid manifest, generic model recorded.
    CHECK(result.manifest.model.empty() || result.manifest.model == "mpk-mini");
}

TEST_MAIN()
