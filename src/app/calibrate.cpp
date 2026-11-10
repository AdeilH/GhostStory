// ghost_story — tools/calibrate.cpp — compiled into ghost_story as the `calibrate`
// command (see main.cpp): turn each hardware knob in turn; the tool records
// which CC each knob emits and writes a profile.toml that the mapping layer
// loads. This makes ALL 8 knobs functional regardless of the MPK's stock
// preset CC map (observed stock maps differ from model defaults).
#include "log/logger.hpp"
#include "midi/alsa_client.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace ghost_story::app {

int cmd_calibrate(char const* output_path) {
    log::Logger::init("", log::Level::Error);
    midi::AlsaClient client;
    if (!client.open("ghost_story-calibrate", true)) {
        std::fprintf(stderr, "ghost_story: ALSA unavailable\n");
        return 1;
    }

    struct CCEvent {
        std::uint8_t cc;
        std::uint8_t val;
        std::uint64_t stamp_ns;
    };
    std::vector<CCEvent> events;
    std::mutex events_m;
    struct Sink : midi::EventSink {
        std::vector<CCEvent>* events;
        std::mutex* m;
        void on_midi(midi::MidiEvent const& ev) override {
            if (ev.type != midi::Type::CC) return;
            std::scoped_lock l(*m);
            events->push_back({static_cast<std::uint8_t>(ev.controller()),
                               static_cast<std::uint8_t>(ev.value()),
                               ev.stamp_ns});
        }
        void on_device_event(midi::DeviceEvent const&) override {}
    } sink;
    sink.events = &events;
    sink.m = &events_m;
    client.set_sink(&sink);
    client.start();

    // Connect any MPK.
    bool connected = false;
    for (auto const& p : midi::AlsaClient::enumerate_input_ports()) {
        if (!midi::mpk_model_from_name(p.client_name).empty()) {
            client.connect_input(p.client, p.port);
            connected = true;
        }
    }
    if (!connected) {
        std::fprintf(stderr,
                     "ghost_story: no MPK Mini attached — connect it and retry\n");
        return 3;
    }

    std::printf(
        "Ghost Story knob calibration\n"
        "=======================\n"
        "Turn each knob when asked. Move it over its FULL range a few\n"
        "times so every position emits. The tool records the CC number.\n\n");

    std::vector<int> knob_cc(8, -1);
    for (int k = 0; k < 8; ++k) {
        std::printf("Knob %d: turn it now (full range, 3 s)... ",
                    k + 1);
        std::fflush(stdout);
        auto const t0 = std::chrono::steady_clock::now();
        // Count CC frequency in the window; the most active NEW cc wins.
        std::vector<std::pair<int, int>> counts;  // (cc, count)
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count() < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<CCEvent> copy;
            {
                std::scoped_lock l(events_m);
                copy.swap(events);
            }
            for (auto const& e : copy) {
                if (e.val == 0 && e.cc == 0) continue;  // filter junk pairs
                if (std::abs(static_cast<int>(e.stamp_ns / 1000000) -
                             static_cast<int>(std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now()
                                     .time_since_epoch())
                                 .count())) > 5000) {
                    continue;  // stale
                }
                bool found = false;
                for (auto& [cc, n] : counts) {
                    if (cc == e.cc) {
                        ++n;
                        found = true;
                    }
                }
                if (!found) counts.emplace_back(e.cc, 1);
            }
        }
        // Pick the most-active CC not already assigned to another knob.
        int best = -1, best_n = 0;
        for (auto const& [cc, n] : counts) {
            bool taken = false;
            for (int prev : knob_cc) {
                if (prev == cc) taken = true;
            }
            if (!taken && n > best_n) {
                best = cc;
                best_n = n;
            }
        }
        if (best < 0 || best_n < 3) {
            std::printf("NOT DETECTED (too little movement) — retry\n");
            --k;
            continue;
        }
        knob_cc[static_cast<std::size_t>(k)] = best;
        std::printf("CC %d (%d events)\n", best, best_n);
    }

    client.stop();
    std::printf("\nKnob map: ");
    for (int i = 0; i < 8; ++i)
        std::printf("knob%d=CC%d ", i + 1, knob_cc[static_cast<std::size_t>(i)]);
    std::printf("\n");

    // Write profile.toml.
    std::string const out_str =
        output_path != nullptr ? std::string{output_path} : std::string{"profile.toml"};
    FILE* f = std::fopen(out_str.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "ghost_story: cannot write %s\n", out_str.c_str());
        return 1;
    }
    std::fprintf(f, "# Generated by `ghost_story calibrate` — knob CC bindings.\n"
                    "[knobs]\n");
    for (int i = 0; i < 8; ++i) {
        std::fprintf(f, "%d.cc = %d\n", i + 1,
                     knob_cc[static_cast<std::size_t>(i)]);
    }
    std::fclose(f);
    std::printf("written: %s\n", out_str.c_str());
    return 0;
}

}  // namespace ghost_story::app
