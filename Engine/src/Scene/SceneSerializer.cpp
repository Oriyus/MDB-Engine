#include "Scene/SceneSerializer.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"
#include "Scene/Prefab.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Engine {

    namespace {

        json WriteSettings(const SceneSettings& s, float ambient) {
            const auto& p = s.Post;
            const auto& sh = s.Shadows;
            return {
                { "environment", { { "map", s.EnvironmentMap },{ "showSky", s.ShowSky },{ "blur", s.SkyBlur },
                                   { "rotation", s.SkyRotation },{ "intensity", ambient } } },
                { "post", { { "exposure", p.Exposure },{ "toneMap", p.ToneMap },{ "gamma", p.Gamma },
                            { "fxaa", p.FXAA },{ "taa", p.TAA },{ "contrast", p.Contrast },{ "saturation", p.Saturation },
                            { "temperature", p.Temperature },{ "tint", p.Tint },{ "shadowTint", p.ShadowTint },
                            { "highlightTint", p.HighlightTint },{ "vignette", p.Vignette },{ "grain", p.Grain } } },
                { "ao", { { "enabled", p.AOEnabled },{ "strength", p.AOStrength },{ "radius", s.AORadius },
                          { "bias", s.AOBias },{ "samples", s.AOSamples } } },
                { "bloom", { { "enabled", p.BloomEnabled },{ "intensity", p.BloomIntensity },
                             { "threshold", s.BloomThreshold },{ "knee", s.BloomKnee },
                             { "radius", s.BloomRadius },{ "maxBrightness", s.BloomMaxBrightness } } },
                { "shadows", { { "enabled", sh.Enabled },{ "pointBias", sh.PointBias },{ "pointFar", sh.PointFar },{ "pointInterval", sh.PointUpdateInterval },
                               { "sunBias", sh.SunBias },{ "sunExtent", sh.SunExtent },{ "sunDistance", sh.SunDistance },
                               { "sunCascades", sh.SunCascades },{ "cascadeCount", sh.CascadeCount },
                               { "shadowDistance", sh.ShadowDistance },{ "cascadeSplit", sh.CascadeSplit } } },
                { "volumetricFog", { { "enabled", s.VolumetricFog.Enabled },{ "density", s.VolumetricFog.Density },
                                     { "heightFalloff", s.VolumetricFog.HeightFalloff },{ "baseHeight", s.VolumetricFog.BaseHeight },
                                     { "color", s.VolumetricFog.Color },{ "anisotropy", s.VolumetricFog.Anisotropy },
                                     { "sunIntensity", s.VolumetricFog.SunIntensity },{ "ambientIntensity", s.VolumetricFog.AmbientIntensity },
                                     { "pointLightIntensity", s.VolumetricFog.PointLightIntensity },
                                     { "noiseScale", s.VolumetricFog.NoiseScale },{ "noiseAmount", s.VolumetricFog.NoiseAmount },
                                     { "windSpeed", s.VolumetricFog.WindSpeed },{ "windDirection", s.VolumetricFog.WindDirection },
                                     { "maxDistance", s.VolumetricFog.MaxDistance },{ "steps", s.VolumetricFog.Steps } } },
                { "physics", { { "gravity", s.Gravity } } },
                { "camera", { { "position", s.CameraPosition },{ "yaw", s.CameraYaw },{ "pitch", s.CameraPitch },
                              { "focusDistance", s.CameraFocusDistance } } },
            };
        }

        // Every section and field is optional: what's missing keeps the value passed in
        void ReadSettings(const json& j, SceneSettings& s, float& ambient) {
            auto section = [&](const char* name) -> const json* {
                auto it = j.find(name);
                return it != j.end() && it->is_object() ? &*it : nullptr;
            };
            auto& p = s.Post;
            auto& sh = s.Shadows;
            if (auto* e = section("environment")) {
                s.EnvironmentMap = e->value("map", s.EnvironmentMap);
                s.ShowSky = e->value("showSky", s.ShowSky);
                s.SkyBlur = e->value("blur", s.SkyBlur);
                s.SkyRotation = e->value("rotation", s.SkyRotation);
                ambient = e->value("intensity", ambient);
            }
            if (auto* e = section("post")) {
                p.Exposure = e->value("exposure", p.Exposure);
                p.ToneMap = std::clamp(e->value("toneMap", p.ToneMap), 0, 2);
                p.Gamma = e->value("gamma", p.Gamma);
                p.FXAA = e->value("fxaa", p.FXAA);
                p.TAA = e->value("taa", p.TAA);
                p.Contrast = e->value("contrast", p.Contrast);
                p.Saturation = e->value("saturation", p.Saturation);
                p.Temperature = e->value("temperature", p.Temperature);
                p.Tint = e->value("tint", p.Tint);
                if (e->contains("shadowTint")) p.ShadowTint = e->at("shadowTint").get<glm::vec3>();
                if (e->contains("highlightTint")) p.HighlightTint = e->at("highlightTint").get<glm::vec3>();
                p.Vignette = e->value("vignette", p.Vignette);
                p.Grain = e->value("grain", p.Grain);
            }
            if (auto* e = section("ao")) {
                p.AOEnabled = e->value("enabled", p.AOEnabled);
                p.AOStrength = e->value("strength", p.AOStrength);
                s.AORadius = e->value("radius", s.AORadius);
                s.AOBias = e->value("bias", s.AOBias);
                s.AOSamples = std::clamp(e->value("samples", s.AOSamples), 4, 32);
            }
            if (auto* e = section("bloom")) {
                p.BloomEnabled = e->value("enabled", p.BloomEnabled);
                p.BloomIntensity = e->value("intensity", p.BloomIntensity);
                s.BloomThreshold = e->value("threshold", s.BloomThreshold);
                s.BloomKnee = e->value("knee", s.BloomKnee);
                s.BloomRadius = e->value("radius", s.BloomRadius);
                s.BloomMaxBrightness = e->value("maxBrightness", s.BloomMaxBrightness);
            }
            if (auto* e = section("shadows")) {
                sh.Enabled = e->value("enabled", sh.Enabled);
                sh.PointBias = e->value("pointBias", sh.PointBias);
                sh.PointFar = e->value("pointFar", sh.PointFar);
                sh.PointUpdateInterval = std::clamp(e->value("pointInterval", sh.PointUpdateInterval), 1, 8);
                sh.SunBias = e->value("sunBias", sh.SunBias);
                sh.SunExtent = e->value("sunExtent", sh.SunExtent);
                sh.SunDistance = e->value("sunDistance", sh.SunDistance);
                sh.SunCascades = e->value("sunCascades", sh.SunCascades);
                sh.CascadeCount = std::clamp(e->value("cascadeCount", sh.CascadeCount), 1, 4);
                sh.ShadowDistance = e->value("shadowDistance", sh.ShadowDistance);
                sh.CascadeSplit = e->value("cascadeSplit", sh.CascadeSplit);
            }
            if (auto* e = section("volumetricFog")) {
                auto& v = s.VolumetricFog;
                v.Enabled = e->value("enabled", v.Enabled);
                v.Density = e->value("density", v.Density);
                v.HeightFalloff = e->value("heightFalloff", v.HeightFalloff);
                v.BaseHeight = e->value("baseHeight", v.BaseHeight);
                if (e->contains("color")) v.Color = e->at("color").get<glm::vec3>();
                v.Anisotropy = e->value("anisotropy", v.Anisotropy);
                v.SunIntensity = e->value("sunIntensity", v.SunIntensity);
                v.AmbientIntensity = e->value("ambientIntensity", v.AmbientIntensity);
                v.PointLightIntensity = e->value("pointLightIntensity", v.PointLightIntensity);
                v.NoiseScale = e->value("noiseScale", v.NoiseScale);
                v.NoiseAmount = e->value("noiseAmount", v.NoiseAmount);
                v.WindSpeed = e->value("windSpeed", v.WindSpeed);
                v.WindDirection = e->value("windDirection", v.WindDirection);
                v.MaxDistance = e->value("maxDistance", v.MaxDistance);
                v.Steps = std::clamp(e->value("steps", v.Steps), 8, 96);
            }
            if (auto* e = section("physics"))
                s.Gravity = e->value("gravity", s.Gravity);
            if (auto* e = section("camera")) {
                if (e->contains("position")) s.CameraPosition = e->at("position").get<glm::vec3>();
                s.CameraYaw = e->value("yaw", s.CameraYaw);
                s.CameraPitch = e->value("pitch", s.CameraPitch);
                s.CameraFocusDistance = e->value("focusDistance", s.CameraFocusDistance);
            }
        }

    } // namespace

    bool SceneSerializer::Save(const std::string& path, const SceneSettings* settings) const {
        json entities = json::array();

        auto& reg = m_Scene.Registry();
        for (auto handle : reg.view<TagComponent>()) {
            Entity e(handle, &m_Scene);
            json j;
            j["name"] = e.Get<TagComponent>().Tag;
            j["transform"] = TransformToJson(e.Get<TransformComponent>());
            j["id"] = m_Scene.GetID(handle);
            if (e.Has<ParentComponent>()) j["parent"] = e.Get<ParentComponent>().Parent;

            // A prefab part is stored as its link (prefab + node) plus only what it overrides. A
            // child part's transform is left out unless it was moved, so moving it in the prefab
            // reaches every instance. The root lists child nodes this instance deleted. If the
            // prefab can't be read right now (or there's no library), a full copy is stored instead,
            // so saving never loses data.
            if (e.Has<PrefabComponent>()) {
                const auto& pc = e.Get<PrefabComponent>();
                j["prefab"] = pc.Path;
                if (pc.Node != 0) j["node"] = pc.Node;
                if (m_Prefabs && m_Prefabs->HasNode(pc.Path, pc.Node)) {
                    json overrides = m_Prefabs->GetOverrides(e);
                    if (!overrides.empty()) j["overrides"] = std::move(overrides);
                    if (pc.Node != 0 && !m_Prefabs->TransformOverridden(e)) j.erase("transform");
                    if (pc.Node == 0) {
                        const auto removed = m_Prefabs->MissingNodes(m_Scene, e);
                        if (!removed.empty()) j["removedNodes"] = removed;
                    }
                }
                else {
                    j.update(ComponentsToJson(e, m_Resources));
                }
            }
            else {
                j.update(ComponentsToJson(e, m_Resources));
            }

            entities.push_back(std::move(j));
        }

        json root = { { "version", 3 },{ "entities", entities } };
        if (settings) {
            root["settings"] = WriteSettings(*settings, m_Scene.AmbientIntensity);
            root["settings"]["wind"] = { { "direction", m_Scene.Wind.Direction },{ "speed", m_Scene.Wind.Speed },{ "strength", m_Scene.Wind.Strength } };
        }

        std::ofstream file(path);
        if (!file) { ENGINE_ERROR("Could not write scene file: {}", path); return false; }
        file << root.dump(2);
        ENGINE_INFO("Saved {} entities to {}", entities.size(), path);
        return true;
    }

    bool SceneSerializer::Load(const std::string& path, SceneSettings* settings) {
        std::ifstream file(path);
        if (!file) { ENGINE_ERROR("Could not open scene file: {}", path); return false; }

        json root;
        try {
            file >> root;
        }
        catch (const std::exception& ex) {
            ENGINE_ERROR("Scene file is not valid JSON: {}", ex.what());
            return false;
        }

        // Parse everything into a temporary list first so a bad file can't wreck the live scene
        Scene temp;   // entities are built here, then the registry is swapped in

        std::vector<std::pair<entt::entity, std::vector<uint64_t>>> instanceRoots;   // synced once all are loaded
        try {
            for (const auto& j : root.at("entities")) {
                Entity e = temp.CreateEntity(j.at("name").get<std::string>());
                if (j.contains("transform")) TransformFromJson(j["transform"], e.Get<TransformComponent>());
                if (j.contains("id")) e.Add<IDComponent>(IDComponent{ j["id"].get<uint64_t>() });   // else the new one stays
                if (j.contains("parent")) e.Add<ParentComponent>(ParentComponent{ j["parent"].get<uint64_t>() });

                const std::string prefab = j.value("prefab", std::string());
                const uint64_t node = j.value("node", uint64_t(0));
                if (!prefab.empty() && !HasComponentKeys(j)) {
                    // A link: the prefab node as it is now, with this part's overrides on top. A child
                    // part without a stored transform takes its node's.
                    const json overrides = j.contains("overrides") ? j["overrides"] : json::object();
                    if (!m_Prefabs || !m_Prefabs->ApplyPrefabTo(e, prefab, node, overrides, !j.contains("transform"))) {
                        ENGINE_WARN("'{}' links to prefab {}, which could not be loaded: kept as an empty entity",
                            e.Get<TagComponent>().Tag, prefab);
                        e.Add<PrefabComponent>(PrefabComponent{ prefab, node });
                    }
                    if (node == 0)
                        instanceRoots.emplace_back(e.GetHandle(), j.value("removedNodes", std::vector<uint64_t>{}));
                }
                else {
                    ComponentsFromJson(j, e, m_Resources);
                    if (!prefab.empty()) e.Add<PrefabComponent>(PrefabComponent{ prefab, node });   // an older full-copy instance
                }
            }
            // Bring every instance's structure up to date with its prefab: parts the prefab gained
            // appear (unless this instance deleted them), parts it lost become plain entities
            if (m_Prefabs)
                for (auto& [h, removed] : instanceRoots) m_Prefabs->SyncInstance(temp, Entity(h, &temp), removed);
        }
        catch (const std::exception& ex) {
            ENGINE_ERROR("Scene file is malformed: {}", ex.what());
            return false;
        }

        // Settings parse into copies too, so a bad value leaves everything as it was
        SceneSettings parsedSettings = settings ? *settings : SceneSettings{};
        float ambient = m_Scene.AmbientIntensity;
        Scene::WindSettings wind = m_Scene.Wind;
        try {
            if (root.contains("settings")) {
                ReadSettings(root["settings"], parsedSettings, ambient);
                if (auto it = root["settings"].find("wind"); it != root["settings"].end() && it->is_object()) {
                    wind.Direction = it->value("direction", wind.Direction);
                    wind.Speed = it->value("speed", wind.Speed);
                    wind.Strength = it->value("strength", wind.Strength);
                }
            }
        }
        catch (const std::exception& ex) {
            ENGINE_ERROR("Scene settings are malformed: {}", ex.what());
            return false;
        }

        // Success: clear the live scene and copy the entities across
        m_Scene.Clear();
        for (auto handle : temp.Registry().view<TagComponent>())
            m_Scene.CreateFromSnapshot(temp.CaptureEntity(handle));

        if (root.contains("settings")) {
            m_Scene.AmbientIntensity = ambient;
            m_Scene.Wind = wind;
            if (settings) *settings = parsedSettings;
        }

        ENGINE_INFO("Loaded scene from {}", path);
        return true;
    }

} // namespace Engine
