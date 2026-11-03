// addu94 — tools/fuzz_midi/fuzz_midi.cpp
//
// MIDI fuzz harness (spec §7.5): fires malformed/garbage MIDI streams at
// the engine through a virtual ALSA port — random bytes, SysEx storms,
// note floods, stuck notes, clock attacks, garbage WITH mid-SysEx
// disconnects. The engine (AlsaClient decode + EventSink pipeline) must
// never crash, leak (ASan in CI covers), or wedge.
//
// Exit 0 = engine alive after the storm; nonzero = failure.
#include "midi/alsa_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using namespace addu94::midi;

namespace {

struct CountingSink : EventSink {
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> sysex{0};
    void on_midi(MidiEvent const&) override { events.fetch_add(1); }
    void on_device_event(DeviceEvent const&) override {}
    void on_sysex_raw(std::uint8_t const*, std::size_t) override {
        sysex.fetch_add(1);
    }
};

void send_raw_bytes(AlsaClient& from, int dest_client, int dest_port,
                    std::vector<std::uint8_t> const& bytes) {
    // Send as a single SysEx event (ALSA wraps raw bytes as SYSEX ext);
    // garbage bytes inside are the point.
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    ev.type = SND_SEQ_EVENT_SYSEX;
    ev.flags |= SND_SEQ_EVENT_LENGTH_VARIABLE;
    ev.data.ext.len = static_cast<unsigned int>(bytes.size());
    ev.data.ext.ptr = const_cast<std::uint8_t*>(bytes.data());
    snd_seq_ev_set_source(&ev, static_cast<unsigned char>(from.virtual_port()));
    snd_seq_ev_set_dest(&ev, static_cast<unsigned char>(dest_client),
                        static_cast<unsigned char>(dest_port));
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(from.raw(), &ev);
}

void send_note(AlsaClient& from, int dest_client, int dest_port, int note,
               int vel, bool on) {
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    ev.type = on ? SND_SEQ_EVENT_NOTEON : SND_SEQ_EVENT_NOTEOFF;
    ev.data.note.note = static_cast<unsigned char>(note);
    ev.data.note.velocity = static_cast<unsigned char>(vel);
    snd_seq_ev_set_source(&ev, static_cast<unsigned char>(from.virtual_port()));
    snd_seq_ev_set_dest(&ev, static_cast<unsigned char>(dest_client),
                        static_cast<unsigned char>(dest_port));
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(from.raw(), &ev);
}

}  // namespace

int main(int argc, char** argv) {
    int const seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    auto const seed = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 12345u;

    AlsaClient engine;
    if (!engine.open("addu94-fuzz-target", true)) {
        std::fprintf(stderr, "no ALSA\n");
        return 2;
    }
    CountingSink sink;
    engine.set_sink(&sink);
    if (!engine.start()) {
        std::fprintf(stderr, "engine start failed\n");
        return 2;
    }
    AlsaClient attacker;
    if (!attacker.open("addu94-fuzz-attacker", true)) return 2;
    if (!engine.connect_input(attacker.client_id(), attacker.virtual_port())) {
        std::fprintf(stderr, "connect failed\n");
        return 2;
    }

    std::mt19937 rng(static_cast<unsigned>(seed));
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::uint64_t garbage_blobs = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        int const kind = static_cast<int>(rng() % 6);
        switch (kind) {
        case 0: {  // random bytes (1..300)
            std::vector<std::uint8_t> blob(1 + rng() % 300);
            for (auto& b : blob) b = static_cast<std::uint8_t>(rng());
            send_raw_bytes(attacker, engine.client_id(),
                           engine.virtual_port(), blob);
            break;
        }
        case 1: {  // SysEx storm: F0 ... (unterminated / re-terminated)
            std::vector<std::uint8_t> blob{0xF0};
            for (int i = 0; i < 8 + static_cast<int>(rng() % 60); ++i)
                blob.push_back(static_cast<std::uint8_t>(rng() % 0x80));
            blob.push_back(rng() % 2 ? 0xF7 : static_cast<std::uint8_t>(rng()));
            send_raw_bytes(attacker, engine.client_id(),
                           engine.virtual_port(), blob);
            break;
        }
        case 2: {  // note flood
            for (int i = 0; i < 64; ++i) {
                send_note(attacker, engine.client_id(),
                          engine.virtual_port(), static_cast<int>(rng() % 128),
                          static_cast<int>(rng() % 128), (rng() % 2) == 0);
            }
            break;
        }
        case 3: {  // stuck notes: N note-ons, no offs
            for (int i = 0; i < 32; ++i) {
                send_note(attacker, engine.client_id(),
                          engine.virtual_port(), 60, 100, true);
            }
            break;
        }
        case 4: {  // clock attack: F1/F8..FC floods
            std::vector<std::uint8_t> blob(64, 0xF8);
            blob[0] = 0xF1; blob[1] = 0xF2;
            send_raw_bytes(attacker, engine.client_id(),
                           engine.virtual_port(), blob);
            break;
        }
        case 5: {  // deep-nested garbage
            std::vector<std::uint8_t> blob(512, 0xF0);
            blob[100] = 0xF7;
            send_raw_bytes(attacker, engine.client_id(),
                           engine.virtual_port(), blob);
            break;
        }
        default: break;
        }
        ++garbage_blobs;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Grace drain, then liveness checks.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    bool const alive = engine.heartbeat_ns() > 0;
    auto const ev = sink.events.load();
    auto const sx = sink.sysex.load();
    std::printf("fuzz: blobs=%llu decoded=%llu sysex=%llu alive=%d "
                "decode_errors=%llu\n",
                static_cast<unsigned long long>(garbage_blobs),
                static_cast<unsigned long long>(ev),
                static_cast<unsigned long long>(sx), alive ? 1 : 0,
                static_cast<unsigned long long>(engine.decode_errors()));
    engine.stop();
    return alive ? 0 : 1;
}
