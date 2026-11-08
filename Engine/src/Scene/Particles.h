#pragma once
#include <string>
#include <vector>

namespace Engine {

    struct ParticleEmitterComponent;

    // Ready-made emitter settings: "Fire", "Smoke", "Sparks", "Magic", "Dust", "Embers".
    // Sets the look and motion; leaves the texture as it is. False for an unknown name.
    bool ApplyParticlePreset(ParticleEmitterComponent& pe, const std::string& name);
    const std::vector<std::string>& ParticlePresetNames();

} // namespace Engine
