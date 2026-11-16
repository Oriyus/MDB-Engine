#include "Renderer/MaterialAsset.h"

#include <algorithm>

using json = nlohmann::json;

namespace Engine {

    const char* MaterialAlphaModeName(AlphaMode m) {
        return m == AlphaMode::Cutout ? "cutout" : m == AlphaMode::Blend ? "blend" : "opaque";
    }
    AlphaMode MaterialAlphaModeFromName(const std::string& s) {
        return s == "cutout" ? AlphaMode::Cutout : s == "blend" ? AlphaMode::Blend : AlphaMode::Opaque;
    }

    namespace {
        // Lenient readers: a wrong type keeps the default, so a hand-edited file never fails as a whole
        void Get(const json& j, const char* k, float& v) { if (auto it = j.find(k); it != j.end() && it->is_number()) v = it->get<float>(); }
        void Get(const json& j, const char* k, bool& v) { if (auto it = j.find(k); it != j.end() && it->is_boolean()) v = it->get<bool>(); }
        void Get(const json& j, const char* k, std::string& v) { if (auto it = j.find(k); it != j.end() && it->is_string()) v = it->get<std::string>(); }
        template <int N>
        void GetVec(const json& j, const char* k, glm::vec<N, float>& v) {
            auto it = j.find(k);
            if (it == j.end() || !it->is_array() || it->size() != N) return;
            for (int i = 0; i < N; i++) if ((*it)[i].is_number()) v[i] = (*it)[i].get<float>();
        }
        json Arr(const glm::vec2& v) { return json::array({ v.x, v.y }); }
        json Arr(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
    }

    MaterialDesc MaterialDescFromJson(const json& j) {
        MaterialDesc d;
        if (!j.is_object()) return d;
        Get(j, "shader", d.Shader);
        GetVec(j, "color", d.Color);
        Get(j, "albedoMap", d.AlbedoMap);
        Get(j, "alpha", d.Alpha);
        std::string mode;
        Get(j, "alphaMode", mode);
        if (!mode.empty()) d.Mode = MaterialAlphaModeFromName(mode);
        Get(j, "alphaCutoff", d.AlphaCutoff);
        Get(j, "doubleSided", d.DoubleSided);
        Get(j, "castShadows", d.CastShadows);
        Get(j, "unlit", d.Unlit);
        Get(j, "metallic", d.Metallic);
        Get(j, "roughness", d.Roughness);
        Get(j, "specular", d.Specular);
        Get(j, "metalRoughMap", d.MetalRoughMap);
        Get(j, "roughnessMap", d.RoughnessMap);
        Get(j, "metallicMap", d.MetallicMap);
        Get(j, "normalMap", d.NormalMap);
        Get(j, "normalStrength", d.NormalStrength);
        Get(j, "normalFlipY", d.NormalFlipY);
        GetVec(j, "emissive", d.Emissive);
        Get(j, "emissiveIntensity", d.EmissiveIntensity);
        Get(j, "emissiveMap", d.EmissiveMap);
        Get(j, "heightMap", d.HeightMap);
        GetVec(j, "uvScale", d.UVScale);
        GetVec(j, "uvOffset", d.UVOffset);
        d.Alpha = std::clamp(d.Alpha, 0.0f, 1.0f);
        d.AlphaCutoff = std::clamp(d.AlphaCutoff, 0.0f, 1.0f);
        d.Metallic = std::clamp(d.Metallic, 0.0f, 1.0f);
        d.Roughness = std::clamp(d.Roughness, 0.0f, 1.0f);
        d.Specular = std::clamp(d.Specular, 0.0f, 1.0f);
        d.EmissiveIntensity = std::max(d.EmissiveIntensity, 0.0f);
        return d;
    }

    json MaterialDescToJson(const MaterialDesc& d, const MaterialDesc* base) {
        const MaterialDesc def;
        const MaterialDesc& b = base ? *base : def;
        const bool all = base == nullptr;
        json j = json::object();
        auto put = [&](const char* k, auto v, auto bv) { if (all || v != bv) j[k] = v; };
        auto putV = [&](const char* k, const auto& v, const auto& bv) { if (all || v != bv) j[k] = Arr(v); };
        put("shader", d.Shader, b.Shader);
        putV("color", d.Color, b.Color);
        put("albedoMap", d.AlbedoMap, b.AlbedoMap);
        put("alpha", d.Alpha, b.Alpha);
        if (all || d.Mode != b.Mode) j["alphaMode"] = MaterialAlphaModeName(d.Mode);
        put("alphaCutoff", d.AlphaCutoff, b.AlphaCutoff);
        put("doubleSided", d.DoubleSided, b.DoubleSided);
        put("castShadows", d.CastShadows, b.CastShadows);
        put("unlit", d.Unlit, b.Unlit);
        put("metallic", d.Metallic, b.Metallic);
        put("roughness", d.Roughness, b.Roughness);
        put("specular", d.Specular, b.Specular);
        put("metalRoughMap", d.MetalRoughMap, b.MetalRoughMap);
        put("roughnessMap", d.RoughnessMap, b.RoughnessMap);
        put("metallicMap", d.MetallicMap, b.MetallicMap);
        put("normalMap", d.NormalMap, b.NormalMap);
        put("normalStrength", d.NormalStrength, b.NormalStrength);
        put("normalFlipY", d.NormalFlipY, b.NormalFlipY);
        putV("emissive", d.Emissive, b.Emissive);
        put("emissiveIntensity", d.EmissiveIntensity, b.EmissiveIntensity);
        put("emissiveMap", d.EmissiveMap, b.EmissiveMap);
        put("heightMap", d.HeightMap, b.HeightMap);
        putV("uvScale", d.UVScale, b.UVScale);
        putV("uvOffset", d.UVOffset, b.UVOffset);
        return j;
    }

} // namespace Engine

namespace Engine {
    const Material& DefaultMaterial() {
        static const Material m = [] { Material d; d.Name = "Default"; return d; }();
        return m;
    }
}
