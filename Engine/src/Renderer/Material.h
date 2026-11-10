#pragma once
#include "Renderer/Texture.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine {

    // How a surface uses alpha. The numbers are stored in scene files: append only.
    //   Opaque: alpha ignored.  Cutout: pixels below the cutoff are dropped (leaves, fences).
    //   Blend: see-through, drawn after the opaque scene, back to front (glass, water, ghosts).
    enum class AlphaMode : int { Opaque = 0, Cutout = 1, Blend = 2 };

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
        float Alpha = 1.0f;                       // multiplies the albedo texture's alpha
        AlphaMode Mode = AlphaMode::Opaque;
        float AlphaCutoff = 0.5f;                 // Cutout only
        bool DoubleSided = false;                 // back faces drawn too, lit from their own side (glTF doubleSided)
        // Specular reflection of a non-metal, 0..1 (glTF KHR_materials_specular specularFactor; Blender's Specular IOR
        // Level exports it): scales the usual 4% reflectance and the bright reflection at grazing angles. 0 = none.
        float Specular = 1.0f;
    };

} // namespace Engine