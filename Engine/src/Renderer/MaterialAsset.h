#pragma once
#include "Renderer/Material.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    struct TerrainSurfaceMaps;

    // A material file, Assets/Materials/<name>.mat (JSON), reusable on any Mesh Renderer part and terrain layer:
    //
    //   { "version": 1, "shader": "standard", "parent": "Materials/stone.mat",
    //     "color": [r, g, b], "albedoMap": "Textures/x.png", "alpha": 1, "alphaMode": "opaque|cutout|blend",
    //     "alphaCutoff": 0.5, "doubleSided": false, "castShadows": true, "unlit": false,
    //     "metallic": 0, "roughness": 0.5, "specular": 1,
    //     "metalRoughMap": "...",                      packed like glTF: G roughness, B metallic (linear)
    //     "roughnessMap": "...", "metallicMap": "...", grey maps, used when there is no metalRoughMap
    //     "normalMap": "...", "normalStrength": 1, "normalFlipY": false,   (OpenGL style unless normalFlipY)
    //     "emissive": [r, g, b], "emissiveIntensity": 1, "emissiveMap": "...",
    //     "heightMap": "...",                          grey; terrain layers blend by it
    //     "uvScale": [1, 1], "uvOffset": [0, 0] }
    //
    // Every key is optional. With a parent, the file holds only what it changes: the parent's values (its own parent's
    // first) are read, then this file's keys replace them, so a variant follows later edits of its parent.
    // "shader" names the shader the values feed: "standard" (lit.frag's PBR) or a shader graph ("Shaders/x.shadergraph",
    // Renderer/ShaderGraph.h), whose params are set in "params": { "Tint": [1, 0, 0], "Albedo": "Textures/x.png" }; with a
    // graph only the render-state values are used (alphaMode, alphaCutoff, doubleSided, castShadows, unlit).
    struct MaterialDesc {
        std::string Shader = "standard";
        glm::vec3 Color{ 1.0f };                  // sRGB-style
        std::string AlbedoMap;
        float Alpha = 1.0f;
        AlphaMode Mode = AlphaMode::Opaque;
        float AlphaCutoff = 0.5f;
        bool DoubleSided = false;
        bool CastShadows = true;
        bool Unlit = false;
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        float Specular = 1.0f;
        std::string MetalRoughMap;
        std::string RoughnessMap, MetallicMap;
        std::string NormalMap;
        float NormalStrength = 1.0f;
        bool NormalFlipY = false;
        glm::vec3 Emissive{ 0.0f };               // linear
        float EmissiveIntensity = 1.0f;
        std::string EmissiveMap;
        std::string HeightMap;
        glm::vec2 UVScale{ 1.0f }, UVOffset{ 0.0f };
        // A shader graph material: the values of the graph's params by name (numbers: a number or a list; textures: a path).
        // A variant's params are merged into its parent's one by one.
        nlohmann::json Params = nlohmann::json::object();
        bool IsGraph() const { return Shader.size() > 12 && Shader.compare(Shader.size() - 12, 12, ".shadergraph") == 0; }
    };

    // The keys a file may hold (besides version and parent), in the order they are written
    MaterialDesc MaterialDescFromJson(const nlohmann::json& j);
    // Every value (sparse = false), or only those that differ from `base` (a variant's file against its parent's values)
    nlohmann::json MaterialDescToJson(const MaterialDesc& d, const MaterialDesc* base = nullptr);
    // Alpha modes by name: "opaque", "cutout", "blend"
    const char* MaterialAlphaModeName(AlphaMode m);
    AlphaMode MaterialAlphaModeFromName(const std::string& s);

    // One loaded material file (ResourceLibrary::LoadMaterial). Shared by every user and changed in place when the
    // file changes or the editor edits it, so all parts and terrain layers using it follow at once.
    struct MaterialAsset {
        std::string Path;                         // relative to Assets, e.g. "Materials/brick.mat"
        nlohmann::json File = nlohmann::json::object();   // the file's own keys (the editor edits these)
        MaterialDesc Desc;                        // resolved: parent values + File
        Material Surface;                         // Desc with its textures loaded: what mesh parts draw with
        std::shared_ptr<const TerrainSurfaceMaps> TerrainMaps;   // normal / roughness / height packed for terrain layers
        bool Missing = false;                     // no such file (drawn magenta); loaded once it appears
        std::string Error;                        // unreadable JSON, a missing or looping parent
        uint32_t Revision = 0;                    // goes up every time it is resolved again
        std::string Parent() const { return File.value("parent", std::string()); }
    };

} // namespace Engine
