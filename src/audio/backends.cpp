// ghost_story — src/audio/backends.cpp
#include "audio/backend.hpp"

#include "audio/alsa_backend.hpp"
#include "audio/pipewire_backend.hpp"
#if defined(GHOST_STORY_HAVE_JACK)
#include "audio/jack_backend.hpp"
#endif

#include <memory>
#include <utility>
#include <vector>

namespace ghost_story::audio {

std::vector<std::unique_ptr<AudioBackend>> make_backends() {
    std::vector<std::unique_ptr<AudioBackend>> backends;
    // Preference order (spec §3): PipeWire native first, then JACK (works
    // live here through PipeWire's pipewire-jack server), then ALSA as the
    // last-resort blocking-writer backend. The JACK backend is compiled in
    // only when the build found the jack headers (CMake defines
    // GHOST_STORY_HAVE_JACK together with linking PkgConfig::JACK); without it
    // the engine silently falls through to ALSA.
    backends.push_back(std::make_unique<PipeWireBackend>());
#if defined(GHOST_STORY_HAVE_JACK)
    backends.push_back(std::make_unique<JackBackend>());
#endif
    backends.push_back(std::make_unique<AlsaBackend>());
    return backends;
}

}  // namespace ghost_story::audio
