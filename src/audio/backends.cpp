// addu94 — src/audio/backends.cpp
#include "audio/backend.hpp"

#include "audio/pipewire_backend.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace addu94::audio {

std::vector<std::unique_ptr<AudioBackend>> make_backends() {
    std::vector<std::unique_ptr<AudioBackend>> backends;
    // Preference order (spec §3): PipeWire native first. JACK and ALSA
    // fallbacks plug in here in a later milestone.
    backends.push_back(std::make_unique<PipeWireBackend>());
    return backends;
}

}  // namespace addu94::audio
