#pragma once
#include "Renderer/Texture.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine {

    struct Material {
        std::string Name;
        glm::vec3 Color{ 1.0f };                  // albedo tint, sRGB-style like the color picker
        std::shared_ptr<Texture2D> Albedo;        // sRGB
        std::shared_ptr<Texture2D> Normal;        // linear, tangent space
        std::shared_ptr<Texture2D> MetalRough;    // linear, glTF layout: G = roughness, B = metallic
        std::shared_ptr<Texture2D> Emissive;      // sRGB
        float Metallic = 0.0f;                    // multiplies the map's B channel
        float Roughness = 0.5f;                   // multiplies the map's G channel
        glm::vec3 EmissiveColor{ 0.0f };          // linear, may exceed 1 (glows)
    };

} // namespace Engine