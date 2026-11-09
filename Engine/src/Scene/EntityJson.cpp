#include "Scene/EntityJson.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"

using json = nlohmann::json;

namespace Engine {

    // Alpha modes are saved by name ("opaque", "cutout", "blend")
    static const char* AlphaModeName(AlphaMode m) {
        return m == AlphaMode::Cutout ? "cutout" : m == AlphaMode::Blend ? "blend" : "opaque";
    }
    static AlphaMode AlphaModeFromName(const std::string& s) {
        return s == "cutout" ? AlphaMode::Cutout : s == "blend" ? AlphaMode::Blend : AlphaMode::Opaque;
    }

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
                { "alphaMode", AlphaModeName(mr.Alpha) },
                { "opacity", mr.Opacity },
                { "alphaCutoff", mr.AlphaCutoff },
                { "castShadows", mr.CastShadows },
                { "static", mr.Static },
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
        if (e.Has<ColliderComponent>()) {
            auto& c = e.Get<ColliderComponent>();
            j["collider"] = { { "shape", (int)c.Shape },{ "size", c.Size },{ "center", c.Center } };
        }
        if (e.Has<ParticleEmitterComponent>()) {
            auto& p = e.Get<ParticleEmitterComponent>();
            j["particles"] = {
                { "emitting", p.Emitting },{ "rate", p.Rate },{ "burst", p.Burst },{ "maxParticles", p.MaxParticles },
                { "lifetime", p.Lifetime },{ "speed", p.Speed },{ "direction", p.Direction },{ "spread", p.Spread },
                { "shape", (int)p.Shape },{ "shapeSize", p.ShapeSize },{ "gravityScale", p.GravityScale },{ "drag", p.Drag },
                { "size", p.Size },{ "endSize", p.EndSize },{ "startColor", p.StartColor },{ "endColor", p.EndColor },
                { "emission", p.Emission },{ "spinSpeed", p.SpinSpeed },
                { "texture", res.TextureName ? res.TextureName(p.Texture) : std::string() },
                { "additive", p.Additive },{ "worldSpace", p.WorldSpace },{ "destroyWhenDone", p.DestroyWhenDone } };
        }
        if (e.Has<FoliageComponent>()) {
            auto& f = e.Get<FoliageComponent>();
            j["foliage"] = {
                { "size", f.Size },{ "density", f.Density },{ "seed", f.Seed },{ "height", f.Height },
                { "heightVariation", f.HeightVariation },{ "width", f.Width },{ "baseColor", f.BaseColor },
                { "tipColor", f.TipColor },{ "dryColor", f.DryColor },{ "dryAmount", f.DryAmount },
                { "flowerAmount", f.FlowerAmount },{ "roughness", f.Roughness },{ "windStrength", f.WindStrength },
                { "windSpeed", f.WindSpeed },{ "windDirection", f.WindDirection },{ "interaction", f.Interaction },
                { "recovery", f.Recovery },{ "drawDistance", f.DrawDistance },{ "avoidStatic", f.AvoidStatic } };
        }
        if (e.Has<AnimatorComponent>()) {
            auto& a = e.Get<AnimatorComponent>();
            j["animator"] = { { "clip", a.Clip },{ "speed", a.Speed },{ "loop", a.Loop },{ "playing", a.Playing },
                              { "blendTime", a.BlendTime } };
            // Only when present, so animators without them keep their old JSON (no prefab overrides appear)
            if (!a.Layers.empty()) {
                json layers = json::array();
                for (const auto& L : a.Layers)
                    layers.push_back({ { "clip", L.Clip },{ "mask", L.Mask },{ "weight", L.Weight },{ "speed", L.Speed },{ "loop", L.Loop },
                                       { "additive", L.Additive } });
                j["animator"]["layers"] = layers;
            }
            if (!a.IK.empty()) {
                json iks = json::array();
                for (const auto& ik : a.IK)
                    iks.push_back({ { "type", ik.Type == IKType::Limb ? "limb" : "look" },{ "bone", ik.Bone },
                                    { "targetId", ik.TargetID },{ "target", ik.Target },{ "weight", ik.Weight },
                                    { "maxAngle", ik.MaxAngle },{ "chain", ik.Chain },{ "pole", ik.Pole } });
                j["animator"]["ik"] = iks;
            }
            if (a.RootMotion || !a.RootBone.empty()) {
                j["animator"]["rootMotion"] = a.RootMotion;
                j["animator"]["rootBone"] = a.RootBone;
            }
            if (!a.Events.empty()) {
                json events = json::array();
                for (const auto& ev : a.Events) events.push_back({ { "clip", ev.Clip },{ "time", ev.Time },{ "name", ev.Name } });
                j["animator"]["events"] = events;
            }
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
            mr.Alpha = AlphaModeFromName(jm.value("alphaMode", std::string("opaque")));
            mr.Opacity = jm.value("opacity", 1.0f);
            mr.AlphaCutoff = jm.value("alphaCutoff", 0.5f);
            mr.CastShadows = jm.value("castShadows", true);
            mr.Static = jm.value("static", false);
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
        if (j.contains("collider")) {
            const auto& jc = j["collider"];
            ColliderComponent c;
            c.Shape = (ColliderShape)jc.value("shape", 0);
            c.Size = jc.at("size").get<glm::vec3>();
            c.Center = jc.at("center").get<glm::vec3>();
            e.Add<ColliderComponent>(c);
        }
        if (j.contains("particles")) {
            const auto& jp = j["particles"];
            ParticleEmitterComponent p;
            p.Emitting = jp.value("emitting", p.Emitting);
            p.Rate = jp.value("rate", p.Rate);
            p.Burst = jp.value("burst", p.Burst);
            p.MaxParticles = jp.value("maxParticles", p.MaxParticles);
            if (jp.contains("lifetime")) p.Lifetime = jp["lifetime"].get<glm::vec2>();
            if (jp.contains("speed")) p.Speed = jp["speed"].get<glm::vec2>();
            if (jp.contains("direction")) p.Direction = jp["direction"].get<glm::vec3>();
            p.Spread = jp.value("spread", p.Spread);
            p.Shape = (EmitShape)jp.value("shape", (int)p.Shape);
            if (jp.contains("shapeSize")) p.ShapeSize = jp["shapeSize"].get<glm::vec3>();
            p.GravityScale = jp.value("gravityScale", p.GravityScale);
            p.Drag = jp.value("drag", p.Drag);
            if (jp.contains("size")) p.Size = jp["size"].get<glm::vec2>();
            p.EndSize = jp.value("endSize", p.EndSize);
            if (jp.contains("startColor")) p.StartColor = jp["startColor"].get<glm::vec4>();
            if (jp.contains("endColor")) p.EndColor = jp["endColor"].get<glm::vec4>();
            p.Emission = jp.value("emission", p.Emission);
            if (jp.contains("spinSpeed")) p.SpinSpeed = jp["spinSpeed"].get<glm::vec2>();
            if (res.FindTexture) p.Texture = res.FindTexture(jp.value("texture", std::string()));
            p.Additive = jp.value("additive", p.Additive);
            p.WorldSpace = jp.value("worldSpace", p.WorldSpace);
            p.DestroyWhenDone = jp.value("destroyWhenDone", p.DestroyWhenDone);
            e.Add<ParticleEmitterComponent>(p);
        }
        if (j.contains("foliage")) {
            const auto& jf = j["foliage"];
            FoliageComponent f;
            if (jf.contains("size")) f.Size = jf["size"].get<glm::vec2>();
            f.Density = jf.value("density", f.Density);
            f.Seed = jf.value("seed", f.Seed);
            f.Height = jf.value("height", f.Height);
            f.HeightVariation = jf.value("heightVariation", f.HeightVariation);
            f.Width = jf.value("width", f.Width);
            if (jf.contains("baseColor")) f.BaseColor = jf["baseColor"].get<glm::vec3>();
            if (jf.contains("tipColor")) f.TipColor = jf["tipColor"].get<glm::vec3>();
            if (jf.contains("dryColor")) f.DryColor = jf["dryColor"].get<glm::vec3>();
            f.DryAmount = jf.value("dryAmount", f.DryAmount);
            f.FlowerAmount = jf.value("flowerAmount", f.FlowerAmount);
            f.Roughness = jf.value("roughness", f.Roughness);
            f.WindStrength = jf.value("windStrength", f.WindStrength);
            f.WindSpeed = jf.value("windSpeed", f.WindSpeed);
            f.WindDirection = jf.value("windDirection", f.WindDirection);
            f.Interaction = jf.value("interaction", f.Interaction);
            f.Recovery = jf.value("recovery", f.Recovery);
            f.DrawDistance = jf.value("drawDistance", f.DrawDistance);
            f.AvoidStatic = jf.value("avoidStatic", f.AvoidStatic);
            e.Add<FoliageComponent>(f);
        }
        if (j.contains("animator")) {
            const auto& ja = j["animator"];
            AnimatorComponent a;
            a.Clip = ja.value("clip", std::string());
            a.Speed = ja.value("speed", 1.0f);
            a.Loop = ja.value("loop", true);
            a.Playing = ja.value("playing", true);
            a.BlendTime = ja.value("blendTime", 0.25f);
            if (ja.contains("layers"))
                for (const auto& jl : ja["layers"]) {
                    AnimLayer L;
                    L.Clip = jl.value("clip", std::string());
                    L.Mask = jl.value("mask", std::string());
                    L.Weight = jl.value("weight", 1.0f);
                    L.Speed = jl.value("speed", 1.0f);
                    L.Loop = jl.value("loop", true);
                    L.Additive = jl.value("additive", false);
                    a.Layers.push_back(L);
                }
            if (ja.contains("ik"))
                for (const auto& ji : ja["ik"]) {
                    AnimIK ik;
                    ik.Type = ji.value("type", std::string("look")) == "limb" ? IKType::Limb : IKType::Look;
                    ik.Bone = ji.value("bone", std::string());
                    ik.TargetID = ji.value("targetId", (uint64_t)0);
                    if (ji.contains("target")) ik.Target = ji["target"].get<glm::vec3>();
                    ik.Weight = ji.value("weight", 1.0f);
                    ik.MaxAngle = ji.value("maxAngle", 70.0f);
                    ik.Chain = ji.value("chain", 2);
                    if (ji.contains("pole")) ik.Pole = ji["pole"].get<glm::vec3>();
                    a.IK.push_back(ik);
                }
            a.RootMotion = ja.value("rootMotion", false);
            a.RootBone = ja.value("rootBone", std::string());
            if (ja.contains("events"))
                for (const auto& je : ja["events"])
                    a.Events.push_back({ je.value("clip", std::string()), je.value("time", 0.0f), je.value("name", std::string()) });
            e.Add<AnimatorComponent>(a);
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
        if (e.Has<ColliderComponent>())         e.Remove<ColliderComponent>();
        if (e.Has<ScriptComponent>())           e.Remove<ScriptComponent>();
        if (e.Has<CameraComponent>())           e.Remove<CameraComponent>();
        if (e.Has<AudioSourceComponent>())      e.Remove<AudioSourceComponent>();
        if (e.Has<AnimatorComponent>())         e.Remove<AnimatorComponent>();
        if (e.Has<ParticleEmitterComponent>())  e.Remove<ParticleEmitterComponent>();
        if (e.Has<FoliageComponent>())          e.Remove<FoliageComponent>();
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
