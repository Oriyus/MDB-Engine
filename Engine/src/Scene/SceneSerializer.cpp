#include "Scene/SceneSerializer.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
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

    bool SceneSerializer::Save(const std::string& path) const {
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
                    { "shininess", mr.Shininess },
                    { "specularStrength", mr.SpecularStrength },
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
            entities.push_back(std::move(j));
        }

        json root = { { "version", 1 },{ "entities", entities } };

        std::ofstream file(path);
        if (!file) { ENGINE_ERROR("Could not write scene file: {}", path); return false; }
        file << root.dump(2);
        ENGINE_INFO("Saved {} entities to {}", entities.size(), path);
        return true;
    }

    bool SceneSerializer::Load(const std::string& path) {
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
                    mr.Shininess = jm.at("shininess").get<float>();
                    mr.SpecularStrength = jm.at("specularStrength").get<float>();
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
            }
        }
        catch (const std::exception& ex) {
            ENGINE_ERROR("Scene file is malformed: {}", ex.what());
            return false;
        }

        // Success: clear the live scene and copy the entities across
        m_Scene.Clear();
        auto& src = temp.Registry();
        for (auto handle : src.view<TagComponent>()) {
            Entity from(handle, &temp);
            Entity to = m_Scene.CreateEntity(from.Get<TagComponent>().Tag);
            to.Get<TransformComponent>() = from.Get<TransformComponent>();
            if (from.Has<MeshRendererComponent>())  to.Add<MeshRendererComponent>(from.Get<MeshRendererComponent>());
            if (from.Has<PointLightComponent>())    to.Add<PointLightComponent>(from.Get<PointLightComponent>());
            if (from.Has<SpinComponent>())          to.Add<SpinComponent>(from.Get<SpinComponent>());
            if (from.Has<OrbitComponent>())         to.Add<OrbitComponent>(from.Get<OrbitComponent>());
            if (from.Has<DirectionalLightComponent>()) to.Add<DirectionalLightComponent>(from.Get<DirectionalLightComponent>());
        }

        ENGINE_INFO("Loaded scene from {}", path);
        return true;
    }

} // namespace Engine