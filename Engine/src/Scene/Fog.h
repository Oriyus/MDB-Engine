#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine {

    // What SceneRenderer needs to draw fog of war: a top-down map over the level (one texel per nav grid
    // column) and the colours. Navigation/FogOfWar computes it; the renderer uploads Texels when Revision
    // changes and darkens every pixel by where it is (its world X/Z, from the depth buffer).
    struct FogOfWarState {
        bool Enabled = false;
        glm::vec2 Origin{ 0.0f };          // world X/Z of the map's corner
        float CellSize = 1.0f;             // metres per texel
        int Width = 0, Depth = 0;          // texels along X and Z
        std::vector<uint8_t> Texels;       // Width * Depth * 2: R = visible now, G = explored (0..255, faded)
        uint32_t Revision = 0;             // bumped whenever Texels change
        glm::vec3 HiddenColor{ 0.0f };     // never seen (sRGB)
        glm::vec3 ExploredColor{ 0.03f, 0.04f, 0.08f };   // seen before, not now (sRGB)
        float ExploredAlpha = 0.6f;        // how much of the explored colour covers the scene
    };

} // namespace Engine
