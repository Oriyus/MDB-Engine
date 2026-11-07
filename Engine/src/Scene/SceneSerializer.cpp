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
                            { "fxaa", p.FXAA } } },
                { "ao", { { "enabled", p.AOEnabled },{ "strength", p.AOStrength },{ "radius", s.AORadius },
                          { "bias", s.AOBias },{ "samples", s.AOSamples } } },
                { "bloom", { { "enabled", p.BloomEnabled },{ "intensity", p.BloomIntensity },
                             { "threshold", s.BloomThreshold },{ "knee", s.BloomKnee },
                             { "radius", s.BloomRadius },{ "maxBrightness", s.BloomMaxBrightness } } },
                { "shadows", { { "enabled", sh.Enabled },{ "pointBias", sh.PointBias },{ "pointFar", sh.PointFar },
                               { "sunBias", sh.SunBias },{ "sunExtent", sh.SunExtent },{ "sunDistance", sh.SunDistance } } },
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
                sh.SunBias = e->value("sunBias", sh.SunBias);
                sh.SunExtent = e->value("sunExtent", sh.SunExtent);
                sh.SunDistance = e->value("sunDistance", sh.SunDistance);
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
        if (settings) root["settings"] = WriteSettings(*settings, m_Scene.AmbientIntensity);

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
        try {
            if (root.contains("settings")) ReadSettings(root["settings"], parsedSettings, ambient);
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
            if (settings) *settings = parsedSettings;
        }

        ENGINE_INFO("Loaded scene from {}", path);
        return true;
    }

} // namespace Engine
