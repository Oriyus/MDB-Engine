#include "Scene/EntityJson.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"

using json = nlohmann::json;

namespace Engine {

    json ComponentsToJson(Entity e, const ResourceRegistry& res) {
        json j = json::object();
        if (e.Has<MeshRendererComponent>()) {
            auto& mr = e.Get<MeshRendererComponent>();
            j["meshRenderer"] = {
                { "geometry", res.GeometryName ? res.GeometryName(mr.Geometry) : std::string() },
                { "texture",  res.TextureName ? res.TextureName(mr.Texture) : std::string() },
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
        if (e.Has<CameraComponent>()) {
            auto& c = e.Get<CameraComponent>();
            j["camera"] = { { "fov", c.FOV },{ "near", c.Near },{ "far", c.Far },{ "primary", c.Primary } };
        }
        if (e.Has<AudioSourceComponent>()) {
            auto& a = e.Get<AudioSourceComponent>();
            j["audio"] = { { "clip", a.Clip },{ "volume", a.Volume },{ "pitch", a.Pitch },{ "loop", a.Loop },
                           { "playOnStart", a.PlayOnStart },{ "spatial", a.Spatial },
                           { "minDistance", a.MinDistance },{ "maxDistance", a.MaxDistance } };
        }
        return j;
    }

    void ComponentsFromJson(const json& j, Entity e, const ResourceRegistry& res) {
        if (j.contains("meshRenderer")) {
            const auto& jm = j["meshRenderer"];
            MeshRendererComponent mr;
            if (res.FindGeometry) mr.Geometry = res.FindGeometry(jm.at("geometry").get<std::string>());
            if (res.FindTexture) mr.Texture = res.FindTexture(jm.at("texture").get<std::string>());
            mr.Color = jm.at("color").get<glm::vec3>();
            mr.LocalOffset = jm.at("localOffset").get<glm::mat4>();
            mr.Metallic = jm.value("metallic", 0.0f);
            mr.Roughness = jm.value("roughness", 0.5f);
            mr.Emissive = jm.contains("emissive") ? jm["emissive"].get<glm::vec3>() : glm::vec3(0.0f);
            mr.Unlit = jm.at("unlit").get<bool>();
            mr.UseModelMaterials = jm.value("useModelMaterials", false);
            e.Add<MeshRendererComponent>(mr);
        }
        if (j.contains("pointLight")) {
            PointLightComponent l;
            l.Color = j["pointLight"].at("color").get<glm::vec3>();
            l.Intensity = j["pointLight"].at("intensity").get<float>();
            l.CastShadows = j["pointLight"].value("castShadows", true);
            e.Add<PointLightComponent>(l);
        }
        if (j.contains("directionalLight")) {
            const auto& jd = j["directionalLight"];
            DirectionalLightComponent d;
            d.Color = jd.at("color").get<glm::vec3>();
            d.Intensity = jd.at("intensity").get<float>();
            d.CastShadows = jd.value("castShadows", true);
            e.Add<DirectionalLightComponent>(d);
        }
        if (j.contains("spin"))
            e.Add<SpinComponent>().Speed = j["spin"].at("speed").get<glm::vec3>();
        if (j.contains("orbit")) {
            const auto& jo = j["orbit"];
            OrbitComponent o;
            o.Center = jo.at("center").get<glm::vec3>();
            o.Radius = jo.at("radius").get<float>();
            o.Height = jo.at("height").get<float>();
            o.Speed = jo.at("speed").get<float>();
            o.Angle = jo.at("angle").get<float>();
            e.Add<OrbitComponent>(o);
        }
        if (j.contains("rigidBody")) {
            const auto& jr = j["rigidBody"];
            RigidBodyComponent rb;
            rb.Type = (BodyType)jr.value("type", 1);
            rb.Shape = (ColliderShape)jr.value("shape", 0);
            rb.Size = jr.at("size").get<glm::vec3>();
            rb.Center = jr.at("center").get<glm::vec3>();
            rb.Mass = jr.value("mass", 1.0f);
            rb.Friction = jr.value("friction", 0.5f);
            rb.Restitution = jr.value("restitution", 0.2f);
            rb.IsTrigger = jr.value("trigger", false);
            e.Add<RigidBodyComponent>(rb);
        }
        if (j.contains("script"))
            e.Add<ScriptComponent>().Path = j["script"].at("path").get<std::string>();
        if (j.contains("camera")) {
            const auto& jc = j["camera"];
            CameraComponent c;
            c.FOV = jc.value("fov", 60.0f);
            c.Near = jc.value("near", 0.1f);
            c.Far = jc.value("far", 500.0f);
            c.Primary = jc.value("primary", true);
            e.Add<CameraComponent>(c);
        }
        if (j.contains("audio")) {
            const auto& ja = j["audio"];
            AudioSourceComponent s;
            s.Clip = ja.value("clip", std::string());
            s.Volume = ja.value("volume", 1.0f);
            s.Pitch = ja.value("pitch", 1.0f);
            s.Loop = ja.value("loop", true);
            s.PlayOnStart = ja.value("playOnStart", true);
            s.Spatial = ja.value("spatial", true);
            s.MinDistance = ja.value("minDistance", 1.0f);
            s.MaxDistance = ja.value("maxDistance", 25.0f);
            e.Add<AudioSourceComponent>(s);
        }
    }

    void ClearComponents(Entity e) {
        if (e.Has<MeshRendererComponent>())     e.Remove<MeshRendererComponent>();
        if (e.Has<PointLightComponent>())       e.Remove<PointLightComponent>();
        if (e.Has<DirectionalLightComponent>()) e.Remove<DirectionalLightComponent>();
        if (e.Has<SpinComponent>())             e.Remove<SpinComponent>();
        if (e.Has<OrbitComponent>())            e.Remove<OrbitComponent>();
        if (e.Has<RigidBodyComponent>())        e.Remove<RigidBodyComponent>();
        if (e.Has<ScriptComponent>())           e.Remove<ScriptComponent>();
        if (e.Has<CameraComponent>())           e.Remove<CameraComponent>();
        if (e.Has<AudioSourceComponent>())      e.Remove<AudioSourceComponent>();
    }

    bool HasComponentKeys(const json& j) {
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "name" && it.key() != "transform" && it.key() != "prefab"
                && it.key() != "id" && it.key() != "parent" && it.key() != "overrides"
                && it.key() != "node" && it.key() != "removedNodes") return true;
        return false;
    }

    json TransformToJson(const TransformComponent& t) {
        return { { "position", t.Position },{ "rotation", t.Rotation },{ "scale", t.Scale } };
    }

    void TransformFromJson(const json& j, TransformComponent& t) {
        t.Position = j.at("position").get<glm::vec3>();
        t.Rotation = j.at("rotation").get<glm::vec3>();
        t.Scale = j.at("scale").get<glm::vec3>();
    }

} // namespace Engine
