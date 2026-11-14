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

    // A terrain's surface (Scene/Terrain.h): up to four layers blended by a weight map, drawn by lit.frag's terrain
    // branch. Scene::BuildDrawList fills one per terrain each frame; DrawBatch::Terrain points at it.
    struct TerrainMaterial {
        uint32_t Splat = 0;                       // GL texture: RGBA = the weights of layers 0..3
        const Texture2D* Layers[4] = {};          // optional; each repeats every Tiling metres
        glm::vec4 Color[4]{ glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f) };   // rgb sRGB tint, a roughness
        glm::vec4 Tiling{ 4.0f };                 // metres per repeat, per layer
        glm::vec4 Variation{ 0.0f };              // stochastic (hex) tiling per layer: 0 plain repeat .. 1 full
        const Texture2D* Surface[4] = {};         // optional packed maps per layer: RG normal, B roughness, A height
        glm::ivec4 Maps{ 0 };                     // which of those each layer has: 1 normal, 2 roughness, 4 height
        glm::vec4 NormalStrength{ 1.0f };
        glm::vec2 Size{ 1.0f };                   // the terrain's metres in X / Z
        float Blend = 0.5f;                       // 0 = plain weights, 1 = height blending (stones poke through grass)
    };

} // namespace Engine