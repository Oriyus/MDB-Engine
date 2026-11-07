#include "Scripting/ScriptEngine.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"
#include "Physics/PhysicsWorld.h"
#include "Audio/AudioEngine.h"
#include "Scene/Prefab.h"
#include "Core/Input.h"
#include "Core/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_PRINT_ERRORS 0          // errors are reported through the engine log instead
#include <sol/sol.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <stdexcept>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

    namespace {

        // What a script sees as an entity. Just a handle: components are looked up on every
        // access, so it can never dangle.
        struct ScriptEntity {
            entt::entity Handle = entt::null;
            Scene* Owner = nullptr;

            bool Valid() const { return Owner && Owner->Registry().valid(Handle); }

            template<typename T>
            T& Get() const {
                if (!Valid()) throw std::runtime_error("entity no longer exists");
                if (T* c = Owner->Registry().try_get<T>(Handle)) return *c;
                throw std::runtime_error(std::string("entity has no ") + typeid(T).name());
            }
            template<typename T>
            bool Has() const { return Valid() && Owner->Registry().all_of<T>(Handle); }
        };

        // Component views handed out by e.mesh / e.light / e.body
        struct MeshRef  { ScriptEntity E; };
        struct LightRef { ScriptEntity E; };
        struct BodyRef  { ScriptEntity E; };
        struct CameraRef { ScriptEntity E; };
        struct AudioRef  { ScriptEntity E; };

        constexpr int kKeyFirst = 32, kKeyCount = 349;   // GLFW_KEY_SPACE .. GLFW_KEY_LAST
        constexpr int kMouseCount = 8;

        BodyType ParseBodyType(const std::string& s) {
            if (s == "static") return BodyType::Static;
            if (s == "kinematic") return BodyType::Kinematic;
            if (s == "dynamic") return BodyType::Dynamic;
            throw std::runtime_error("unknown body type '" + s + "' (static, dynamic, kinematic)");
        }
        const char* BodyTypeName(BodyType t) {
            return t == BodyType::Static ? "static" : t == BodyType::Kinematic ? "kinematic" : "dynamic";
        }

    } // namespace

    struct ScriptInstance {
        std::string Path;
        sol::environment Env;
        sol::object Self;
        sol::protected_function OnUpdate, OnDestroy;
        sol::protected_function OnCollisionBegin, OnCollisionEnd, OnTriggerEnter, OnTriggerExit;
        bool Enabled = true;
    };

    struct ScriptEngine::Impl {
        std::string AssetDir;
        PhysicsWorld* Physics = nullptr;
        AudioEngine* Audio = nullptr;
        PrefabLibrary* Prefabs = nullptr;
        ResourceRegistry Resources;
        Scene* ActiveScene = nullptr;
        float Time = 0.0f;

        // Declared before Instances so the instances (which hold Lua references) die first
        std::unique_ptr<sol::state> Lua;
        std::unordered_map<entt::entity, ScriptInstance> Instances;
        std::unordered_set<entt::entity> Failed;          // failed to load: don't retry every frame
        std::vector<entt::entity> PendingDestroy;         // e:destroy() requests, handled when nothing iterates
        std::vector<entt::entity> PendingRestart;         // e:add_script() on an entity that may already run one
        std::vector<std::string> Errors;
        std::vector<ScriptText> Texts;                    // ui.text calls of the current frame
        bool RestartRequested = false;

        std::array<bool, kKeyCount> KeyNow{}, KeyPrev{};
        std::array<bool, kMouseCount> MouseNow{}, MousePrev{};

        void BindApi();
        void SampleInput();
        void StartInstance(entt::entity handle, const std::string& path);
        void DestroyInstance(entt::entity handle, bool callOnDestroy);
        void StartNewInstances(entt::registry& reg);
        void FlushDestroys(Scene& scene);
        bool Check(const sol::protected_function_result& r, ScriptInstance& inst, const char* what);
        void ReportError(const std::string& msg);

        ScriptEntity Wrap(entt::entity h) const { return ScriptEntity{ h, ActiveScene }; }
        sol::object MakeEntity(entt::entity h) const { return sol::make_object(*Lua, Wrap(h)); }
    };

    // ---------------------------------------------------------------- API
    void ScriptEngine::Impl::BindApi() {
        sol::state& lua = *Lua;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);

        // ---- vec3 (a copy: `local p = self.position; p.y = 2; self.position = p`)
        lua.new_usertype<glm::vec3>("vec3",
            sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
            "x", &glm::vec3::x,
            "y", &glm::vec3::y,
            "z", &glm::vec3::z,
            "length", [](const glm::vec3& v) { return glm::length(v); },
            "normalize", [](const glm::vec3& v) { float l = glm::length(v); return l > 0.0f ? v / l : glm::vec3(0.0f); },
            "dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
            "cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); },
            sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
            sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
            sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
            sol::meta_function::multiplication, sol::overload(
                [](const glm::vec3& a, float s) { return a * s; },
                [](float s, const glm::vec3& a) { return a * s; },
                [](const glm::vec3& a, const glm::vec3& b) { return a * b; }),
            sol::meta_function::division, [](const glm::vec3& a, float s) { return a / s; },
            sol::meta_function::equal_to, [](const glm::vec3& a, const glm::vec3& b) { return a == b; },
            sol::meta_function::to_string, [](const glm::vec3& v) {
                return "vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
            });

        // ---- component views
        lua.new_usertype<MeshRef>("Mesh", sol::no_constructor,
            "color", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Color; },
                [](const MeshRef& m, const glm::vec3& v) { m.E.Get<MeshRendererComponent>().Color = v; }),
            "emissive", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Emissive; },
                [](const MeshRef& m, const glm::vec3& v) { m.E.Get<MeshRendererComponent>().Emissive = v; }),
            "metallic", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Metallic; },
                [](const MeshRef& m, float v) { m.E.Get<MeshRendererComponent>().Metallic = v; }),
            "roughness", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Roughness; },
                [](const MeshRef& m, float v) { m.E.Get<MeshRendererComponent>().Roughness = v; }),
            "unlit", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Unlit; },
                [](const MeshRef& m, bool v) { m.E.Get<MeshRendererComponent>().Unlit = v; }),
            "geometry", sol::property(
                [this](const MeshRef& m) {
                    const auto& g = m.E.Get<MeshRendererComponent>().Geometry;
                    return Resources.GeometryName ? Resources.GeometryName(g) : std::string();
                },
                [this](const MeshRef& m, const std::string& name) {
                    auto g = Resources.FindGeometry ? Resources.FindGeometry(name) : nullptr;
                    if (!g) throw std::runtime_error("unknown geometry '" + name + "'");
                    m.E.Get<MeshRendererComponent>().Geometry = g;
                }));

        lua.new_usertype<LightRef>("Light", sol::no_constructor,
            "color", sol::property(
                [](const LightRef& l) { return l.E.Get<PointLightComponent>().Color; },
                [](const LightRef& l, const glm::vec3& v) { l.E.Get<PointLightComponent>().Color = v; }),
            "intensity", sol::property(
                [](const LightRef& l) { return l.E.Get<PointLightComponent>().Intensity; },
                [](const LightRef& l, float v) { l.E.Get<PointLightComponent>().Intensity = v; }),
            "shadows", sol::property(
                [](const LightRef& l) { return l.E.Get<PointLightComponent>().CastShadows; },
                [](const LightRef& l, bool v) { l.E.Get<PointLightComponent>().CastShadows = v; }));

        lua.new_usertype<BodyRef>("Body", sol::no_constructor,
            "velocity", sol::property(
                [this](const BodyRef& b) { b.E.Get<RigidBodyComponent>(); return Physics->GetLinearVelocity(b.E.Handle); },
                [this](const BodyRef& b, const glm::vec3& v) { b.E.Get<RigidBodyComponent>(); Physics->SetLinearVelocity(b.E.Handle, v); }),
            "angular_velocity", sol::property(
                [this](const BodyRef& b) { b.E.Get<RigidBodyComponent>(); return Physics->GetAngularVelocity(b.E.Handle); },
                [this](const BodyRef& b, const glm::vec3& v) { b.E.Get<RigidBodyComponent>(); Physics->SetAngularVelocity(b.E.Handle, v); }),
            "type", sol::readonly_property([](const BodyRef& b) { return std::string(BodyTypeName(b.E.Get<RigidBodyComponent>().Type)); }),
            "trigger", sol::readonly_property([](const BodyRef& b) { return b.E.Get<RigidBodyComponent>().IsTrigger; }),
            "mass", sol::readonly_property([](const BodyRef& b) { return b.E.Get<RigidBodyComponent>().Mass; }),
            "add_force", [this](const BodyRef& b, const glm::vec3& f) { b.E.Get<RigidBodyComponent>(); Physics->AddForce(b.E.Handle, f); },
            "add_impulse", [this](const BodyRef& b, const glm::vec3& i) { b.E.Get<RigidBodyComponent>(); Physics->AddImpulse(b.E.Handle, i); });

        // ---- Entity
        auto dirOf = [](const ScriptEntity& e, const glm::vec3& local) {
            return glm::quat(e.Get<TransformComponent>().Rotation) * local;
        };

        lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor,
            "valid", &ScriptEntity::Valid,
            "id", sol::readonly_property([](const ScriptEntity& e) { return (uint32_t)entt::to_integral(e.Handle); }),
            "name", sol::property(
                [](const ScriptEntity& e) { return e.Get<TagComponent>().Tag; },
                [](const ScriptEntity& e, const std::string& s) { e.Get<TagComponent>().Tag = s; }),
            "position", sol::property(
                [](const ScriptEntity& e) { return e.Get<TransformComponent>().Position; },
                [](const ScriptEntity& e, const glm::vec3& v) { e.Get<TransformComponent>().Position = v; }),
            "rotation", sol::property(
                [](const ScriptEntity& e) { return e.Get<TransformComponent>().Rotation; },
                [](const ScriptEntity& e, const glm::vec3& v) { e.Get<TransformComponent>().Rotation = v; }),
            "scale", sol::property(
                [](const ScriptEntity& e) { return e.Get<TransformComponent>().Scale; },
                [](const ScriptEntity& e, const glm::vec3& v) { e.Get<TransformComponent>().Scale = v; }),
            "forward", sol::readonly_property([dirOf](const ScriptEntity& e) { return dirOf(e, { 0.0f, 0.0f, -1.0f }); }),
            "right", sol::readonly_property([dirOf](const ScriptEntity& e) { return dirOf(e, { 1.0f, 0.0f, 0.0f }); }),
            "up", sol::readonly_property([dirOf](const ScriptEntity& e) { return dirOf(e, { 0.0f, 1.0f, 0.0f }); }),

            "mesh", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<MeshRef> {
                if (e.Has<MeshRendererComponent>()) return MeshRef{ e };
                return sol::nullopt;
            }),
            "light", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<LightRef> {
                if (e.Has<PointLightComponent>()) return LightRef{ e };
                return sol::nullopt;
            }),
            "body", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<BodyRef> {
                if (e.Has<RigidBodyComponent>()) return BodyRef{ e };
                return sol::nullopt;
            }),
            "camera", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<CameraRef> {
                if (e.Has<CameraComponent>()) return CameraRef{ e };
                return sol::nullopt;
            }),
            "add_camera", [](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                CameraComponent c;
                if (opts) {
                    c.FOV = opts->get_or<float>("fov", c.FOV);
                    c.Near = opts->get_or<float>("near", c.Near);
                    c.Far = opts->get_or<float>("far", c.Far);
                    c.Primary = opts->get_or<bool>("primary", c.Primary);
                }
                e.Owner->Registry().emplace_or_replace<CameraComponent>(e.Handle, c);
                return CameraRef{ e };
            },
            // Turn so local -Z (forward) points at target. `up` defaults to world up.
            "look_at", [](const ScriptEntity& e, const glm::vec3& target, sol::optional<glm::vec3> up) {
                auto& t = e.Get<TransformComponent>();
                const glm::vec3 d = target - t.Position;
                if (glm::dot(d, d) < 1e-10f) return;
                const glm::vec3 dir = glm::normalize(d);
                glm::vec3 u = up ? glm::normalize(*up) : glm::vec3(0.0f, 1.0f, 0.0f);
                if (std::abs(glm::dot(dir, u)) > 0.999f) u = glm::vec3(0.0f, 0.0f, 1.0f);   // looking straight up/down
                t.Rotation = glm::eulerAngles(glm::quatLookAt(dir, u));
            },
            "audio", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<AudioRef> {
                if (e.Has<AudioSourceComponent>()) return AudioRef{ e };
                return sol::nullopt;
            }),
            "add_audio", [this](const ScriptEntity& e, const std::string& clip, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                AudioSourceComponent a;
                a.Clip = clip;
                if (opts) {
                    a.Volume = opts->get_or<float>("volume", a.Volume);
                    a.Pitch = opts->get_or<float>("pitch", a.Pitch);
                    a.Loop = opts->get_or<bool>("loop", a.Loop);
                    a.Spatial = opts->get_or<bool>("spatial", a.Spatial);
                    a.PlayOnStart = opts->get_or<bool>("play", a.PlayOnStart);
                    a.MinDistance = opts->get_or<float>("min_distance", a.MinDistance);
                    a.MaxDistance = opts->get_or<float>("max_distance", a.MaxDistance);
                }
                e.Owner->Registry().emplace_or_replace<AudioSourceComponent>(e.Handle, a);
                if (Audio && a.PlayOnStart) Audio->PlaySource(e.Handle);   // also restarts a replaced source
                return AudioRef{ e };
            },

            "has", [](const ScriptEntity& e, const std::string& kind) {
                if (!e.Valid()) return false;
                if (kind == "mesh")   return e.Has<MeshRendererComponent>();
                if (kind == "body")   return e.Has<RigidBodyComponent>();
                if (kind == "light")  return e.Has<PointLightComponent>();
                if (kind == "sun")    return e.Has<DirectionalLightComponent>();
                if (kind == "script") return e.Has<ScriptComponent>();
                if (kind == "spin")   return e.Has<SpinComponent>();
                if (kind == "orbit")  return e.Has<OrbitComponent>();
                if (kind == "camera") return e.Has<CameraComponent>();
                if (kind == "audio")  return e.Has<AudioSourceComponent>();
                throw std::runtime_error("unknown component kind '" + kind + "'");
            },
            "remove", [](const ScriptEntity& e, const std::string& kind) {
                if (!e.Valid()) return;
                auto& reg = e.Owner->Registry();
                if (kind == "mesh")        reg.remove<MeshRendererComponent>(e.Handle);
                else if (kind == "body")   reg.remove<RigidBodyComponent>(e.Handle);
                else if (kind == "light")  reg.remove<PointLightComponent>(e.Handle);
                else if (kind == "sun")    reg.remove<DirectionalLightComponent>(e.Handle);
                else if (kind == "script") reg.remove<ScriptComponent>(e.Handle);
                else if (kind == "spin")   reg.remove<SpinComponent>(e.Handle);
                else if (kind == "orbit")  reg.remove<OrbitComponent>(e.Handle);
                else if (kind == "camera") reg.remove<CameraComponent>(e.Handle);
                else if (kind == "audio")  reg.remove<AudioSourceComponent>(e.Handle);
                else throw std::runtime_error("unknown component kind '" + kind + "'");
            },

            "add_mesh", [this](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                MeshRendererComponent mr;
                const std::string geo = opts ? opts->get_or<std::string>("geometry", "cube") : "cube";
                mr.Geometry = Resources.FindGeometry ? Resources.FindGeometry(geo) : nullptr;
                if (!mr.Geometry) throw std::runtime_error("unknown geometry '" + geo + "'");
                if (opts) {
                    const std::string tex = opts->get_or<std::string>("texture", "");
                    if (!tex.empty()) {
                        mr.Texture = Resources.FindTexture ? Resources.FindTexture(tex) : nullptr;
                        if (!mr.Texture) throw std::runtime_error("unknown texture '" + tex + "'");
                    }
                    mr.Color = opts->get_or<glm::vec3>("color", mr.Color);
                    mr.Emissive = opts->get_or<glm::vec3>("emissive", mr.Emissive);
                    mr.Metallic = opts->get_or<float>("metallic", mr.Metallic);
                    mr.Roughness = opts->get_or<float>("roughness", mr.Roughness);
                    mr.Unlit = opts->get_or<bool>("unlit", mr.Unlit);
                }
                e.Owner->Registry().emplace_or_replace<MeshRendererComponent>(e.Handle, mr);
                return MeshRef{ e };
            },
            "add_body", [this](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                RigidBodyComponent rb;
                if (opts) {
                    rb.Type = ParseBodyType(opts->get_or<std::string>("type", "dynamic"));
                    const std::string shape = opts->get_or<std::string>("shape", "box");
                    if (shape == "sphere") rb.Shape = ColliderShape::Sphere;
                    else if (shape != "box") throw std::runtime_error("unknown shape '" + shape + "' (box, sphere)");
                    sol::object size = (*opts)["size"];
                    if (size.is<glm::vec3>())  rb.Size = size.as<glm::vec3>();
                    else if (size.is<float>()) rb.Size = glm::vec3(size.as<float>());
                    rb.Center = opts->get_or<glm::vec3>("center", rb.Center);
                    rb.Mass = opts->get_or<float>("mass", rb.Mass);
                    rb.Friction = opts->get_or<float>("friction", rb.Friction);
                    rb.Restitution = opts->get_or<float>("bounciness", rb.Restitution);
                    rb.IsTrigger = opts->get<sol::optional<bool>>("trigger").value_or(false);
                    rb.InitialVelocity = opts->get<sol::optional<glm::vec3>>("velocity").value_or(glm::vec3(0.0f));
                }
                e.Owner->Registry().emplace_or_replace<RigidBodyComponent>(e.Handle, rb);
                if (Physics) Physics->Invalidate(e.Handle);   // rebuilt from the new settings on next use
                return BodyRef{ e };
            },
            "add_light", [](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                PointLightComponent l;
                if (opts) {
                    l.Color = opts->get_or<glm::vec3>("color", l.Color);
                    l.Intensity = opts->get_or<float>("intensity", l.Intensity);
                    l.CastShadows = opts->get_or<bool>("shadows", l.CastShadows);
                }
                e.Owner->Registry().emplace_or_replace<PointLightComponent>(e.Handle, l);
                return LightRef{ e };
            },
            "add_script", [this](const ScriptEntity& e, const std::string& path) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                e.Owner->Registry().emplace_or_replace<ScriptComponent>(e.Handle, ScriptComponent{ path });
                // The old instance may be the one running right now: swap it out once nothing iterates
                PendingRestart.push_back(e.Handle);
            },

            "clone", [this](const ScriptEntity& e) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                return Wrap(e.Owner->DuplicateEntity(Entity(e.Handle, e.Owner)).GetHandle());
            },
            "destroy", [this](const ScriptEntity& e) { if (e.Valid()) PendingDestroy.push_back(e.Handle); },
            sol::meta_function::equal_to, [](const ScriptEntity& a, const ScriptEntity& b) { return a.Handle == b.Handle; },
            sol::meta_function::to_string, [](const ScriptEntity& e) {
                return e.Valid() ? "Entity(" + e.Get<TagComponent>().Tag + ")" : std::string("Entity(destroyed)");
            });

        // ---- scene
        sol::table scene = lua.create_named_table("scene");
        scene.set_function("find", [this](const std::string& name) -> sol::optional<ScriptEntity> {
            auto& reg = ActiveScene->Registry();
            for (auto h : reg.view<TagComponent>())
                if (reg.get<TagComponent>(h).Tag == name) return Wrap(h);
            return sol::nullopt;
        });
        scene.set_function("find_all", [this](const std::string& name) {
            auto& reg = ActiveScene->Registry();
            sol::table out = Lua->create_table();
            int n = 1;
            for (auto h : reg.view<TagComponent>())
                if (reg.get<TagComponent>(h).Tag == name) out[n++] = Wrap(h);
            return out;
        });
        scene.set_function("spawn", [this](sol::optional<std::string> name, sol::optional<glm::vec3> pos) {
            Entity e = ActiveScene->CreateEntity(name.value_or("Entity"));
            if (pos) e.Get<TransformComponent>().Position = *pos;
            return Wrap(e.GetHandle());
        });
        // A prefab instance ("coin" or "Prefabs/coin.json"). nil (and a warning) if the prefab is missing.
        scene.set_function("spawn_prefab", [this](const std::string& name, sol::optional<glm::vec3> pos) -> sol::optional<ScriptEntity> {
            if (!Prefabs) return sol::nullopt;
            Entity e = Prefabs->Instantiate(*ActiveScene, PrefabLibrary::Resolve(name));
            if (!e) return sol::nullopt;
            if (pos) e.Get<TransformComponent>().Position = *pos;
            return Wrap(e.GetHandle());
        });
        scene.set_function("time", [this]() { return Time; });
        scene.set_function("restart", [this]() { RestartRequested = true; });   // the app does Stop + Play next frame
        scene.set_function("raycast", [this](const glm::vec3& origin, const glm::vec3& dir, float maxDist,
            sol::optional<ScriptEntity> ignore) -> sol::object {
            RayHit hit;
            if (!Physics || !Physics->Raycast(origin, dir, maxDist, hit, ignore ? ignore->Handle : entt::null))
                return sol::make_object(*Lua, sol::lua_nil);
            return Lua->create_table_with("entity", Wrap(hit.Entity), "point", hit.Point,
                "normal", hit.Normal, "distance", hit.Distance);
        });

        // ---- ui: text drawn over the viewport this frame. x, y are 0..1 across the viewport
        sol::table ui = lua.create_named_table("ui");
        ui.set_function("text", [this](const std::string& text, float x, float y, sol::optional<sol::table> opts) {
            ScriptText t;
            t.Text = text;
            t.X = x;
            t.Y = y;
            if (opts) {
                t.Size = opts->get_or<float>("size", t.Size);
                t.Color = opts->get_or<glm::vec3>("color", t.Color);
                const std::string align = opts->get_or<std::string>("align", "left");
                t.Align = align == "center" ? 1 : align == "right" ? 2 : 0;
            }
            if (Texts.size() < 256) Texts.push_back(std::move(t));
        });

        // ---- audio: one-shots, and the AudioSource view handed out by e.audio
        sol::table audio = lua.create_named_table("audio");
        auto soundOpts = [](const sol::optional<sol::table>& o, float& volume, float& pitch) {
            volume = o ? o->get<sol::optional<float>>("volume").value_or(1.0f) : 1.0f;
            pitch = o ? o->get<sol::optional<float>>("pitch").value_or(1.0f) : 1.0f;
        };
        audio.set_function("play", [this, soundOpts](const std::string& clip, sol::optional<sol::table> opts) {
            float v, p;
            soundOpts(opts, v, p);
            if (Audio) Audio->Play(clip, v, p);
        });
        audio.set_function("play_at", [this, soundOpts](const std::string& clip, const glm::vec3& pos, sol::optional<sol::table> opts) {
            float v, p;
            soundOpts(opts, v, p);
            if (Audio) Audio->PlayAt(clip, pos, v, p);
        });

        lua.new_usertype<AudioRef>("AudioSource", sol::no_constructor,
            "clip", sol::property(
                [](const AudioRef& a) { return a.E.Get<AudioSourceComponent>().Clip; },
                [](const AudioRef& a, const std::string& v) { a.E.Get<AudioSourceComponent>().Clip = v; }),
            "volume", sol::property(
                [](const AudioRef& a) { return a.E.Get<AudioSourceComponent>().Volume; },
                [](const AudioRef& a, float v) { a.E.Get<AudioSourceComponent>().Volume = v; }),
            "pitch", sol::property(
                [](const AudioRef& a) { return a.E.Get<AudioSourceComponent>().Pitch; },
                [](const AudioRef& a, float v) { a.E.Get<AudioSourceComponent>().Pitch = v; }),
            "loop", sol::property(
                [](const AudioRef& a) { return a.E.Get<AudioSourceComponent>().Loop; },
                [](const AudioRef& a, bool v) { a.E.Get<AudioSourceComponent>().Loop = v; }),
            "playing", sol::readonly_property([this](const AudioRef& a) {
                a.E.Get<AudioSourceComponent>();
                return Audio && Audio->IsSourcePlaying(a.E.Handle);
            }),
            "play", [this](const AudioRef& a) { a.E.Get<AudioSourceComponent>(); if (Audio) Audio->PlaySource(a.E.Handle); },
            "stop", [this](const AudioRef& a) { a.E.Get<AudioSourceComponent>(); if (Audio) Audio->StopSource(a.E.Handle); });

        // ---- camera usertype
        lua.new_usertype<CameraRef>("Camera", sol::no_constructor,
            "fov", sol::property(
                [](const CameraRef& c) { return c.E.Get<CameraComponent>().FOV; },
                [](const CameraRef& c, float v) { c.E.Get<CameraComponent>().FOV = v; }),
            "primary", sol::property(
                [](const CameraRef& c) { return c.E.Get<CameraComponent>().Primary; },
                [](const CameraRef& c, bool v) { c.E.Get<CameraComponent>().Primary = v; }));

        // ---- input (sampled once per frame, so pressed/released are exact frame edges)
        sol::table input = lua.create_named_table("input");
        auto keyIndex = [](int key) {
            if (key < kKeyFirst || key >= kKeyCount) throw std::runtime_error("invalid key code " + std::to_string(key));
            return key;
        };
        auto mouseIndex = [](int b) {
            if (b < 0 || b >= kMouseCount) throw std::runtime_error("invalid mouse button " + std::to_string(b));
            return b;
        };
        input.set_function("key_down", [this, keyIndex](int k) { return KeyNow[keyIndex(k)]; });
        input.set_function("key_pressed", [this, keyIndex](int k) { int i = keyIndex(k); return KeyNow[i] && !KeyPrev[i]; });
        input.set_function("key_released", [this, keyIndex](int k) { int i = keyIndex(k); return !KeyNow[i] && KeyPrev[i]; });
        input.set_function("mouse_down", [this, mouseIndex](int b) { return MouseNow[mouseIndex(b)]; });
        input.set_function("mouse_pressed", [this, mouseIndex](int b) { int i = mouseIndex(b); return MouseNow[i] && !MousePrev[i]; });
        input.set_function("mouse_position", []() { auto [x, y] = Input::GetMousePosition(); return std::make_tuple(x, y); });

        sol::table key = lua.create_named_table("Key",
            "Space", 32, "Enter", 257, "Escape", 256, "Tab", 258, "Backspace", 259, "Delete", 261,
            "Right", 262, "Left", 263, "Down", 264, "Up", 265,
            "LeftShift", 340, "LeftCtrl", 341, "LeftAlt", 342, "RightShift", 344, "RightCtrl", 345);
        for (char c = 'A'; c <= 'Z'; c++) key[std::string(1, c)] = (int)c;
        for (int i = 0; i <= 9; i++)      key["Num" + std::to_string(i)] = 48 + i;
        for (int i = 1; i <= 12; i++)     key["F" + std::to_string(i)] = 289 + i;
        lua.create_named_table("Mouse", "Left", Mouse::Left, "Right", Mouse::Right, "Middle", Mouse::Middle);

        // ---- shared: one table every script sees (globals are per entity), for game-wide state
        lua["shared"] = lua.create_table();

        // ---- logging: log(...) and print(...) both go to the game log, args joined with spaces
        lua.set_function("__engine_log", [](const std::string& s) { LOG_INFO("[lua] {}", s); });
        lua.script(R"(
            function log(...)
                local parts = {}
                for i = 1, select('#', ...) do parts[i] = tostring((select(i, ...))) end
                __engine_log(table.concat(parts, ' '))
            end
            print = log
        )");
    }

    // ---------------------------------------------------------------- instances
    void ScriptEngine::Impl::ReportError(const std::string& msg) {
        ENGINE_ERROR("{}", msg);
        Errors.push_back(msg);
        if (Errors.size() > 8) Errors.erase(Errors.begin());
    }

    bool ScriptEngine::Impl::Check(const sol::protected_function_result& r, ScriptInstance& inst, const char* what) {
        if (r.valid()) return true;
        sol::error err = r;
        ReportError(std::string("[script] ") + inst.Path + " (" + what + "): " + err.what());
        inst.Enabled = false;   // one error report, not one per frame
        return false;
    }

    void ScriptEngine::Impl::SampleInput() {
        KeyPrev = KeyNow;
        MousePrev = MouseNow;
        for (int k = kKeyFirst; k < kKeyCount; k++) KeyNow[k] = Input::IsKeyDown(k);
        for (int b = 0; b < kMouseCount; b++)       MouseNow[b] = Input::IsMouseButtonDown(b);
    }

    void ScriptEngine::Impl::StartInstance(entt::entity handle, const std::string& path) {
        if (path.empty()) { Failed.insert(handle); return; }

        ScriptInstance inst;
        inst.Path = path;
        inst.Env = sol::environment(*Lua, sol::create, Lua->globals());   // own globals, falls back to shared ones

        auto loaded = Lua->safe_script_file(AssetDir + path, inst.Env, sol::script_pass_on_error);
        if (!loaded.valid()) {
            sol::error err = loaded;
            ReportError("[script] " + path + " (load): " + err.what());
            Failed.insert(handle);
            return;
        }

        inst.Self = MakeEntity(handle);
        inst.OnUpdate = inst.Env["on_update"];
        inst.OnDestroy = inst.Env["on_destroy"];
        inst.OnCollisionBegin = inst.Env["on_collision_begin"];
        inst.OnCollisionEnd = inst.Env["on_collision_end"];
        inst.OnTriggerEnter = inst.Env["on_trigger_enter"];
        inst.OnTriggerExit = inst.Env["on_trigger_exit"];

        sol::protected_function onCreate = inst.Env["on_create"];
        auto& stored = Instances.emplace(handle, std::move(inst)).first->second;
        if (onCreate.valid()) Check(onCreate(stored.Self), stored, "on_create");
    }

    void ScriptEngine::Impl::DestroyInstance(entt::entity handle, bool callOnDestroy) {
        auto it = Instances.find(handle);
        if (it == Instances.end()) return;
        if (callOnDestroy && it->second.Enabled && it->second.OnDestroy.valid())
            Check(it->second.OnDestroy(it->second.Self), it->second, "on_destroy");
        Instances.erase(handle);   // by key: on_destroy may have touched the map
    }

    void ScriptEngine::Impl::StartNewInstances(entt::registry& reg) {
        // Collect first: on_create may spawn entities, which changes the view
        std::vector<std::pair<entt::entity, std::string>> todo;
        for (auto h : reg.view<ScriptComponent>())
            if (!Instances.contains(h) && !Failed.contains(h)) todo.emplace_back(h, reg.get<ScriptComponent>(h).Path);
        for (auto& [h, path] : todo)
            if (reg.valid(h)) StartInstance(h, path);
    }

    void ScriptEngine::Impl::FlushDestroys(Scene& scene) {
        // on_destroy may destroy more entities, so loop until nothing is left
        while (!PendingDestroy.empty()) {
            auto pending = std::move(PendingDestroy);
            PendingDestroy.clear();
            for (auto h : pending) {
                if (!scene.Registry().valid(h)) continue;
                DestroyInstance(h, true);
                scene.DestroyEntity(Entity(h, &scene));
            }
        }

        // Swapped scripts: end the old instance; StartNewInstances starts the new file next frame
        auto restart = std::move(PendingRestart);
        PendingRestart.clear();
        for (auto h : restart) {
            DestroyInstance(h, scene.Registry().valid(h));
            Failed.erase(h);
        }
    }

    // ---------------------------------------------------------------- ScriptEngine
    ScriptEngine::ScriptEngine(std::string assetDir, PhysicsWorld* physics, AudioEngine* audio) : m_Impl(std::make_unique<Impl>()) {
        m_Impl->AssetDir = std::move(assetDir);
        m_Impl->Physics = physics;
        m_Impl->Audio = audio;
    }

    ScriptEngine::~ScriptEngine() = default;

    void ScriptEngine::SetResources(const ResourceRegistry& resources) { m_Impl->Resources = resources; }
    void ScriptEngine::SetPrefabs(PrefabLibrary* prefabs) { m_Impl->Prefabs = prefabs; }

    bool ScriptEngine::IsRunning() const { return m_Impl->Lua != nullptr; }
    uint32_t ScriptEngine::InstanceCount() const { return (uint32_t)m_Impl->Instances.size(); }
    const std::vector<std::string>& ScriptEngine::RecentErrors() const { return m_Impl->Errors; }
    const std::vector<ScriptText>& ScriptEngine::Texts() const { return m_Impl->Texts; }

    bool ScriptEngine::ConsumeRestartRequest() {
        const bool r = m_Impl->RestartRequested;
        m_Impl->RestartRequested = false;
        return r;
    }

    void ScriptEngine::Start(Scene& scene) {
        Impl& m = *m_Impl;
        if (m.Lua) Stop(scene);

        m.ActiveScene = &scene;
        m.Time = 0.0f;
        m.Errors.clear();
        m.Lua = std::make_unique<sol::state>();
        m.BindApi();
        m.SampleInput();
        m.KeyPrev = m.KeyNow;       // keys already held when Play starts don't count as pressed
        m.MousePrev = m.MouseNow;

        // Start everything now so on_create runs before the first frame
        m.StartNewInstances(scene.Registry());
        m.FlushDestroys(scene);
        ENGINE_INFO("Scripts started: {} running", m.Instances.size());
    }

    void ScriptEngine::Update(Scene& scene, float dt) {
        Impl& m = *m_Impl;
        if (!m.Lua) return;
        m.ActiveScene = &scene;
        m.Time += dt;
        m.Texts.clear();            // a paused game keeps showing the last frame's text
        m.SampleInput();
        auto& reg = scene.Registry();

        // Drop instances whose entity was deleted from C++ (editor Delete, G) or lost its script
        for (auto it = m.Instances.begin(); it != m.Instances.end();) {
            if (!reg.valid(it->first) || !reg.all_of<ScriptComponent>(it->first)) it = m.Instances.erase(it);
            else ++it;
        }

        // Start scripted entities that appeared while playing (spawned, cloned, add_script)
        m.StartNewInstances(reg);

        // Snapshot the handles: on_update may add or remove instances (add_script, clone + start)
        std::vector<entt::entity> handles;
        handles.reserve(m.Instances.size());
        for (auto& [h, inst] : m.Instances) handles.push_back(h);
        for (auto h : handles) {
            auto it = m.Instances.find(h);
            if (it == m.Instances.end() || !it->second.Enabled || !it->second.OnUpdate.valid()) continue;
            m.Check(it->second.OnUpdate(it->second.Self, dt), it->second, "on_update");
        }

        m.FlushDestroys(scene);
    }

    void ScriptEngine::DispatchCollisions(Scene& scene) {
        Impl& m = *m_Impl;
        if (!m.Lua || !m.Physics) return;
        m.ActiveScene = &scene;

        // Copy: a callback can't change this frame's events, but be safe against re-entry anyway
        const std::vector<CollisionEvent> events = m.Physics->Events();

        auto deliver = [&](entt::entity self, entt::entity other, const CollisionEvent& ev, bool selfIsA) {
            auto it = m.Instances.find(self);
            if (it == m.Instances.end() || !it->second.Enabled) return;
            if (!scene.Registry().valid(self)) return;
            ScriptInstance& inst = it->second;
            sol::object o = m.MakeEntity(other);

            if (ev.Trigger) {
                auto& fn = ev.Begin ? inst.OnTriggerEnter : inst.OnTriggerExit;
                if (fn.valid()) m.Check(fn(inst.Self, o), inst, ev.Begin ? "on_trigger_enter" : "on_trigger_exit");
            }
            else if (ev.Begin) {
                if (!inst.OnCollisionBegin.valid()) return;
                // Event normal points A -> B; scripts get it pointing from other to self
                const glm::vec3 n = selfIsA ? -ev.Normal : ev.Normal;
                sol::table hit = m.Lua->create_table_with("point", ev.Point, "normal", n);
                m.Check(inst.OnCollisionBegin(inst.Self, o, hit), inst, "on_collision_begin");
            }
            else if (inst.OnCollisionEnd.valid()) {
                m.Check(inst.OnCollisionEnd(inst.Self, o), inst, "on_collision_end");
            }
        };

        for (const auto& ev : events) {
            deliver(ev.A, ev.B, ev, true);
            deliver(ev.B, ev.A, ev, false);
        }
        m.FlushDestroys(scene);
    }

    void ScriptEngine::Stop(Scene& scene) {
        Impl& m = *m_Impl;
        if (!m.Lua) return;
        m.ActiveScene = &scene;

        std::vector<entt::entity> handles;
        for (auto& [h, inst] : m.Instances) handles.push_back(h);
        for (auto h : handles)
            m.DestroyInstance(h, scene.Registry().valid(h));

        m.Instances.clear();
        m.Failed.clear();
        m.PendingDestroy.clear();   // the scene is about to be restored anyway
        m.PendingRestart.clear();
        m.Texts.clear();
        m.RestartRequested = false;
        m.Lua.reset();
        m.ActiveScene = nullptr;
    }

    void ScriptEngine::Reload(Scene& scene) {
        if (!m_Impl->Lua) return;
        const float time = m_Impl->Time;
        Stop(scene);
        Start(scene);
        m_Impl->Time = time;
        ENGINE_INFO("Scripts reloaded");
    }

} // namespace Engine
