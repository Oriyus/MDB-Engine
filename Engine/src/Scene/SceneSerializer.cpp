#include "Scene/SceneSerializer.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace glm {
    // Lets nlohmann/json read and write glm vectors as [x, y, z]
    static void to_json(json& j, const vec3& v) { j = json::array({ v.x, v.y, v.z }); }
    static void from_json(const json& j, vec3& v) { v = vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()); }
    static void to_json(json& j, const mat4& m) {
        j = json::array();
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) j.push_back(m[c][r]);
    }
    static void from_json(const json& j, mat4& m) {
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) m[c][r] = j.at(c * 4 + r).get<float>();
    }
}

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

            auto& t = e.Get<TransformComponent>();
            j["transform"] = { { "position", t.Position },{ "rotation", t.Rotation },{ "scale", t.Scale } };

            if (e.Has<MeshRendererComponent>()) {
                auto& mr = e.Get<MeshRendererComponent>();
                j["meshRenderer"] = {
                    { "geometry", m_Resources.GeometryName(mr.Geometry) },
                    { "texture",  m_Resources.TextureName(mr.Texture) },
                    { "color", mr.Color },
                    { "localOffset", mr.LocalOffset },
                    { "metallic", mr.Metallic },
                    { "roughness", mr.Roughness },
                    { "emissive", mr.Emissive },
                    { "unlit", mr.Unlit },
                    { "useModelMaterials", mr.UseModelMaterials },
                };
            }
            if (e.Has<PointLightComponent>()) {
                auto& l = e.Get<PointLightComponent>();
                j["pointLight"] = { { "color", l.Color },{ "intensity", l.Intensity },{ "castShadows", l.CastShadows } };
            }
            if (e.Has<DirectionalLightComponent>()) {
                auto& d = e.Get<DirectionalLightComponent>();
                j["directionalLight"] = { { "color", d.Color },{ "intensity", d.Intensity },{ "castShadows", d.CastShadows } };
            }
            if (e.Has<SpinComponent>())
                j["spin"] = { { "speed", e.Get<SpinComponent>().Speed } };
            if (e.Has<OrbitComponent>()) {
                auto& o = e.Get<OrbitComponent>();
                j["orbit"] = { { "center", o.Center },{ "radius", o.Radius },{ "height", o.Height },
                               { "speed", o.Speed },{ "angle", o.Angle } };
            }
            if (e.Has<RigidBodyComponent>()) {
                auto& rb = e.Get<RigidBodyComponent>();
                j["rigidBody"] = { { "type", (int)rb.Type },{ "shape", (int)rb.Shape },{ "size", rb.Size },
                                   { "center", rb.Center },{ "mass", rb.Mass },
                                   { "friction", rb.Friction },{ "restitution", rb.Restitution },
                                   { "trigger", rb.IsTrigger } };
            }
            if (e.Has<ScriptComponent>())
                j["script"] = { { "path", e.Get<ScriptComponent>().Path } };
            entities.push_back(std::move(j));
        }

        json root = { { "version", 2 },{ "entities", entities } };
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
        struct Loaded { Entity entity; };
        Scene temp;   // entities are built here, then the registry is swapped in

        try {
            for (const auto& j : root.at("entities")) {
                Entity e = temp.CreateEntity(j.at("name").get<std::string>());

                auto& t = e.Get<TransformComponent>();
                const auto& jt = j.at("transform");
                t.Position = jt.at("position").get<glm::vec3>();
                t.Rotation = jt.at("rotation").get<glm::vec3>();
                t.Scale = jt.at("scale").get<glm::vec3>();

                if (j.contains("meshRenderer")) {
                    const auto& jm = j["meshRenderer"];
                    auto& mr = e.Add<MeshRendererComponent>();
                    mr.Geometry = m_Resources.FindGeometry(jm.at("geometry").get<std::string>());
                    mr.Texture = m_Resources.FindTexture(jm.at("texture").get<std::string>());
                    mr.Color = jm.at("color").get<glm::vec3>();
                    mr.LocalOffset = jm.at("localOffset").get<glm::mat4>();
                    mr.Metallic = jm.value("metallic", 0.0f);
                    mr.Roughness = jm.value("roughness", 0.5f);
                    mr.Emissive = jm.contains("emissive") ? jm["emissive"].get<glm::vec3>() : glm::vec3(0.0f);
                    mr.Unlit = jm.at("unlit").get<bool>();
                    mr.UseModelMaterials = jm.value("useModelMaterials", false);
                }
                if (j.contains("pointLight")) {
                    auto& l = e.Add<PointLightComponent>();
                    l.Color = j["pointLight"].at("color").get<glm::vec3>();
                    l.Intensity = j["pointLight"].at("intensity").get<float>();
                    l.CastShadows = j["pointLight"].value("castShadows", true);
                }
                if (j.contains("directionalLight")) {
                    auto& d = e.Add<DirectionalLightComponent>();
                    const auto& jd = j["directionalLight"];
                    d.Color = jd.at("color").get<glm::vec3>();
                    d.Intensity = jd.at("intensity").get<float>();
                    d.CastShadows = jd.value("castShadows", true);
                }
                if (j.contains("spin"))
                    e.Add<SpinComponent>().Speed = j["spin"].at("speed").get<glm::vec3>();
                if (j.contains("orbit")) {
                    auto& o = e.Add<OrbitComponent>();
                    const auto& jo = j["orbit"];
                    o.Center = jo.at("center").get<glm::vec3>();
                    o.Radius = jo.at("radius").get<float>();
                    o.Height = jo.at("height").get<float>();
                    o.Speed = jo.at("speed").get<float>();
                    o.Angle = jo.at("angle").get<float>();
                }
                if (j.contains("rigidBody")) {
                    auto& rb = e.Add<RigidBodyComponent>();
                    const auto& jr = j["rigidBody"];
                    rb.Type = (BodyType)jr.value("type", 1);
                    rb.Shape = (ColliderShape)jr.value("shape", 0);
                    rb.Size = jr.at("size").get<glm::vec3>();
                    rb.Center = jr.at("center").get<glm::vec3>();
                    rb.Mass = jr.value("mass", 1.0f);
                    rb.Friction = jr.value("friction", 0.5f);
                    rb.Restitution = jr.value("restitution", 0.2f);
                    rb.IsTrigger = jr.value("trigger", false);
                }
                if (j.contains("script"))
                    e.Add<ScriptComponent>().Path = j["script"].at("path").get<std::string>();
            }
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