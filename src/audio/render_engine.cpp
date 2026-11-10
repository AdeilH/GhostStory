// ghost_story — src/audio/render_engine.cpp
//
// Offline renderer (see render_engine.hpp). Walks the clip timeline
// sample-exactly: events are placed at llround(tick * (60/bpm) / PPQ * sr)
// inside each loop repetition, and each 512-frame block is split at event
// offsets so note_on/note_off land on their exact sample. FX runs per
// block in place (delay/chorus state carries across blocks, matching the
// live engine's behavior), then the block is scaled by track_volume.

#include "audio/render_engine.hpp"

#include <algorithm>
#include <cmath>

namespace ghost_story::audio {
namespace {

constexpr double kPPQ = 96.0;   // music::kPPQ as double
constexpr std::uint32_t kBlock = 512;

struct ScheduledEvent {
    std::uint64_t pos;      // absolute sample position (across all loops)
    std::uint8_t channel;
    std::uint8_t note;
    float velocity;         // 0..1
    bool on;
};

}  // namespace

RenderResult render_track(RenderRequest const& req, int loop_count) {
    RenderResult out;
    music::Clip const* clip = req.clip;
    if (clip == nullptr || loop_count <= 0 || clip->length_ticks <= 0) {
        return out;
    }
    double const sr = (req.sample_rate >= 8000.0 && req.sample_rate <= 192000.0)
                          ? req.sample_rate
                          : 48000.0;
    double const bpm = (req.tempo_bpm >= 20.0 && req.tempo_bpm <= 333.0)
                           ? req.tempo_bpm
                           : 120.0;
    double const sec_per_tick = (60.0 / bpm) / kPPQ;
    double const ticks_to_samples = sec_per_tick * sr;

    std::int64_t const len_ticks = clip->length_ticks;
    auto const loop_samples = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(len_ticks) * ticks_to_samples));
    if (loop_samples == 0) {
        return out;
    }
    auto const loops = static_cast<std::uint64_t>(loop_count);
    std::uint64_t const total = loop_samples * loops;

    // Deterministic event schedule: events sorted by tick (stable, so
    // equal ticks keep clip order), then repeated per loop in order.
    // Positions are monotonic across the whole timeline.
    std::vector<music::ClipEvent const*> sorted;
    sorted.reserve(clip->events.size());
    for (auto const& e : clip->events) {
        sorted.push_back(&e);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](music::ClipEvent const* a, music::ClipEvent const* b) {
                         return a->tick < b->tick;
                     });

    // Placement: the clip's tick 0 sits at start_tick — exported audio
    // matches what the live engine plays (silence before, loop after).
    std::uint64_t const start_offset = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(clip->start_tick) * ticks_to_samples));

    std::vector<ScheduledEvent> evs;
    evs.reserve(sorted.size() * static_cast<std::size_t>(loop_count));
    for (std::uint64_t l = 0; l < loops; ++l) {
        std::uint64_t const base = start_offset + l * loop_samples;
        for (music::ClipEvent const* e : sorted) {
            auto const pos = static_cast<std::uint64_t>(std::llround(
                static_cast<double>(e->tick) * ticks_to_samples));
            if (pos >= loop_samples) {
                continue;  // event at/past the loop end: drop, keeps exact loop length
            }
            evs.push_back(ScheduledEvent{
                base + pos, e->channel, e->note,
                static_cast<float>(e->velocity) / 127.0f, e->on});
        }
    }

    auto const total_with_offset = total + start_offset;
    out.left.assign(static_cast<std::size_t>(total_with_offset), 0.0f);
    out.right.assign(static_cast<std::size_t>(total_with_offset), 0.0f);

    Synth synth;
    synth.set_sample_rate(sr);
    synth.set_params(req.synth_params);
    FxChain fx;
    fx.set_sample_rate(sr);
    fx.set_tempo(bpm);
    fx.set_params(req.fx);

    float const vol = req.track_volume;
    std::size_t ev_i = 0;
    for (std::uint64_t off = 0; off < total; off += kBlock) {
        std::uint64_t const remain = total - off;
        std::uint32_t const n =
            remain >= kBlock ? kBlock : static_cast<std::uint32_t>(remain);
        std::uint64_t const block_end = off + n;

        // Render in sub-segments split at exact event offsets.
        std::uint32_t done = 0;
        while (ev_i < evs.size() && evs[ev_i].pos < block_end) {
            auto const rel = static_cast<std::uint32_t>(evs[ev_i].pos - off);
            if (rel > done) {
                synth.render(out.left.data() + off + done,
                             out.right.data() + off + done, rel - done);
                done = rel;
            }
            if (evs[ev_i].on) {
                synth.note_on(evs[ev_i].channel, evs[ev_i].note,
                              evs[ev_i].velocity);
            } else {
                synth.note_off(evs[ev_i].channel, evs[ev_i].note);
            }
            ++ev_i;
        }
        if (done < n) {
            synth.render(out.left.data() + off + done,
                         out.right.data() + off + done, n - done);
        }

        fx.process(out.left.data() + off, out.right.data() + off, n);
        if (vol != 1.0f) {
            for (std::uint32_t i = 0; i < n; ++i) {
                out.left[off + i] *= vol;
                out.right[off + i] *= vol;
            }
        }
    }
    return out;
}

RenderResult mix_down(std::vector<RenderResult> const& stems) {
    RenderResult out;
    std::size_t frames = 0;
    for (auto const& s : stems) {
        frames = std::max(frames, s.left.size());
    }
    out.left.assign(frames, 0.0f);
    out.right.assign(frames, 0.0f);
    for (auto const& s : stems) {
        for (std::size_t i = 0; i < s.left.size(); ++i) {
            out.left[i] += s.left[i];
        }
        for (std::size_t i = 0; i < s.right.size(); ++i) {
            out.right[i] += s.right[i];
        }
    }
    return out;
}

}  // namespace ghost_story::audio
