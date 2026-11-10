#include "Scripting/ScriptEngine.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"
#include "Physics/PhysicsWorld.h"
#include "Audio/AudioEngine.h"
#include "Scene/Prefab.h"
#include "Renderer/Model.h"
#include "Renderer/RenderStats.h"
#include "Scene/Particles.h"
#include "Navigation/NavGrid.h"
#include "Navigation/FogOfWar.h"
#include "Scene/EntityJson.h"          // glm <-> json
#include "Runtime/UILayout.h"
#include "Core/Input.h"
#include "Core/Log.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_PRINT_ERRORS 0          // errors are reported through the engine log instead
#include <tuple>
#include <cmath>
#include <sol/sol.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
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

        const char* AlphaModeToString(AlphaMode m) {
            return m == AlphaMode::Cutout ? "cutout" : m == AlphaMode::Blend ? "blend" : "opaque";
        }
        AlphaMode AlphaModeFromString(const std::string& s) {
            if (s == "cutout") return AlphaMode::Cutout;
            if (s == "blend") return AlphaMode::Blend;
            if (s != "opaque") throw std::runtime_error("alpha_mode must be \"opaque\", \"cutout\" or \"blend\"");
            return AlphaMode::Opaque;
        }
        struct LightRef { ScriptEntity E; };
        struct BodyRef  { ScriptEntity E; };
        struct CameraRef { ScriptEntity E; };
        struct AudioRef  { ScriptEntity E; };
        struct AnimatorRef { ScriptEntity E; };
        struct ParticlesRef { ScriptEntity E; };

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
        sol::protected_function OnCollisionBegin, OnCollisionEnd, OnTriggerEnter, OnTriggerExit, OnAnimEvent;
        sol::protected_function OnSave, OnLoad, OnEvent;
        bool Enabled = true;
    };

    struct ScriptEngine::Impl {
        std::string AssetDir;                   // the project's Assets
        std::string EngineDir;                  // the engine's files (Scripts/Lib)
        PhysicsWorld* Physics = nullptr;
        AudioEngine* Audio = nullptr;
        PrefabLibrary* Prefabs = nullptr;
        NavGrid* Nav = nullptr;
        FogOfWar* Fog = nullptr;
        ScriptHost* Host = nullptr;
        nlohmann::json PendingRestore;                    // SetRestore: applied by the next Start
        bool HasPendingRestore = false, PendingLoading = false;
        nlohmann::json RestoreScripts = nlohmann::json::object();   // uid -> on_load data, during Start
        bool Loading = false;                             // scene.is_loading()
        NavSettings NavConfig;                            // the last nav.bake settings (lazy bakes use them)
        bool NavTried = false;                            // a lazy bake ran (and may have failed): don't retry every call
        float ViewAspect = 16.0f / 9.0f;
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
        struct QueuedEvent { std::string Name; sol::object Data; };
        std::vector<QueuedEvent> Events;                  // events.emit, delivered after this frame's on_update
        void DispatchEvents();
        void LoadLibraries();
        std::vector<ScriptUIItem> UI;                     // ui.* calls of the current frame
        float PtrX = 0.0f, PtrY = 0.0f;                   // pointer over the game view, 0..1
        bool PtrInside = false, PtrDown = false, PtrClicked = false;
        float PtrWheel = 0.0f;
        bool PtrRightDown = false;
        UIContext UIC;                                    // laid-out ui (panels, labels, slots...)
        sol::object DragPayload;                          // the value of the slot being dragged (after Lua: dies first)
        bool SimPointer = false;                          // ui.simulate_pointer is on
        UIPointerState Sim;
        bool UIWarned = false;
        bool QuitRequested = false;
        bool RestartRequested = false;

        std::array<bool, kKeyCount> KeyNow{}, KeyPrev{};
        std::array<bool, kMouseCount> MouseNow{}, MousePrev{};

        void BindApi();
        void BindNavigation();
        void BindSaves();
        nlohmann::json ToJson(const sol::object& o, int depth = 0);
        sol::object FromJson(const nlohmann::json& j);
        bool EnsureNav();
        bool ScreenRay(float x, float y, glm::vec3& origin, glm::vec3& dir) const;
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
            // opacity below 1 on an opaque mesh makes it see-through (alpha_mode "blend")
            "opacity", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Opacity; },
                [](const MeshRef& m, float v) {
                    auto& mr = m.E.Get<MeshRendererComponent>();
                    mr.Opacity = std::clamp(v, 0.0f, 1.0f);
                    if (mr.Opacity < 1.0f && mr.Alpha == AlphaMode::Opaque) mr.Alpha = AlphaMode::Blend;
                }),
            "alpha_mode", sol::property(
                [](const MeshRef& m) { return std::string(AlphaModeToString(m.E.Get<MeshRendererComponent>().Alpha)); },
                [](const MeshRef& m, const std::string& v) { m.E.Get<MeshRendererComponent>().Alpha = AlphaModeFromString(v); }),
            "alpha_cutoff", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().AlphaCutoff; },
                [](const MeshRef& m, float v) { m.E.Get<MeshRendererComponent>().AlphaCutoff = v; }),
            // static: merged with other static parts of the same material when Play starts (set it before Play)
            "static", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Static; },
                [](const MeshRef& m, bool v) { m.E.Get<MeshRendererComponent>().Static = v; }),
            "wind", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Wind; },
                [](const MeshRef& m, float v) { m.E.Get<MeshRendererComponent>().Wind = std::max(v, 0.0f); }),
            "specular", sol::property(           // 0..1: reflection of a non-metal (0 = matte, no sheen)
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().Specular; },
                [](const MeshRef& m, float v) { m.E.Get<MeshRendererComponent>().Specular = std::clamp(v, 0.0f, 1.0f); }),
            "double_sided", sol::property(       // back faces drawn too (a plane seen from behind)
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().DoubleSided; },
                [](const MeshRef& m, bool v) { m.E.Get<MeshRendererComponent>().DoubleSided = v; }),
            "cast_shadows", sol::property(
                [](const MeshRef& m) { return m.E.Get<MeshRendererComponent>().CastShadows; },
                [](const MeshRef& m, bool v) { m.E.Get<MeshRendererComponent>().CastShadows = v; }),
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
        // World-space direction of a local axis (parents included)
        auto dirOf = [](const ScriptEntity& e, const glm::vec3& local) {
            e.Get<TransformComponent>();                     // throws if the entity is gone
            glm::vec3 p, s;
            glm::quat q;
            Scene::Decompose(e.Owner->GetWorldMatrix(e.Handle), p, q, s);
            return glm::normalize(q * local);
        };

        lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor,
            "valid", &ScriptEntity::Valid,
            "id", sol::readonly_property([](const ScriptEntity& e) { return (uint32_t)entt::to_integral(e.Handle); }),
            "uid", sol::readonly_property([](const ScriptEntity& e) { return (int64_t)e.Owner->GetID(e.Handle); }),
            "transient", sol::property(
                [](const ScriptEntity& e) { return e.Has<TransientComponent>(); },
                [](const ScriptEntity& e, bool v) {
                    if (!e.Valid()) return;
                    if (v) e.Owner->Registry().emplace_or_replace<TransientComponent>(e.Handle);
                    else e.Owner->Registry().remove<TransientComponent>(e.Handle);
                }),
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

            // ---- hierarchy: position/rotation/scale above are local to the parent
            "world_position", sol::property(
                [](const ScriptEntity& e) { e.Get<TransformComponent>(); return glm::vec3(e.Owner->GetWorldMatrix(e.Handle)[3]); },
                [](const ScriptEntity& e, const glm::vec3& v) { e.Get<TransformComponent>(); e.Owner->SetWorldPosition(e.Handle, v); }),
            "parent", sol::property(
                [this](const ScriptEntity& e) -> sol::optional<ScriptEntity> {
                    e.Get<TransformComponent>();
                    const entt::entity p = e.Owner->GetParent(e.Handle);
                    if (p == entt::null) return sol::nullopt;
                    return Wrap(p);
                },
                [](const ScriptEntity& e, sol::object parent) {   // nil = no parent; keeps the world placement
                    e.Get<TransformComponent>();
                    entt::entity p = entt::null;
                    if (parent.is<ScriptEntity>()) {
                        p = parent.as<ScriptEntity>().Handle;
                        if (!e.Owner->Registry().valid(p)) throw std::runtime_error("parent entity no longer exists");
                    }
                    else if (parent.valid() && parent.get_type() != sol::type::lua_nil) {
                        throw std::runtime_error("parent must be an entity or nil");
                    }
                    if (!e.Owner->SetParent(e.Handle, p, true)) throw std::runtime_error("that would make a loop of parents");
                }),
            "children", [this](const ScriptEntity& e) {
                e.Get<TransformComponent>();
                sol::table out = Lua->create_table();
                int n = 1;
                for (auto c : e.Owner->GetChildren(e.Handle)) out[n++] = Wrap(c);
                return out;
            },

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
            // Shown or hidden (runtime, not saved): the entity and every mesh below it; hidden ones cast no shadow.
            // Reading it says whether the entity's own mesh (or, without one, its first child's) is shown.
            "visible", sol::property(
                [](const ScriptEntity& e) {
                    if (!e.Valid()) return false;
                    std::vector<entt::entity> todo{ e.Handle };
                    for (size_t i = 0; i < todo.size(); i++) {
                        if (auto* mr = e.Owner->Registry().try_get<MeshRendererComponent>(todo[i])) return !mr->Hidden;
                        for (entt::entity child : e.Owner->GetChildren(todo[i])) todo.push_back(child);
                    }
                    return true;
                },
                [](const ScriptEntity& e, bool v) {
                    if (!e.Valid()) return;
                    std::vector<entt::entity> todo{ e.Handle };
                    for (size_t i = 0; i < todo.size(); i++) {
                        if (auto* mr = e.Owner->Registry().try_get<MeshRendererComponent>(todo[i])) mr->Hidden = !v;
                        for (entt::entity child : e.Owner->GetChildren(todo[i])) todo.push_back(child);
                    }
                }),
            // A hit flash: the entity and every mesh below it glow `color` and fade back over `seconds`
            "flash", [](const ScriptEntity& e, sol::optional<glm::vec3> color, sol::optional<float> seconds, sol::optional<float> strength) {
                if (!e.Valid()) return;
                const glm::vec3 c = glm::pow(glm::clamp(color.value_or(glm::vec3(1.0f)), glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f))
                    * std::max(0.0f, strength.value_or(3.0f));
                const float t = std::max(0.01f, seconds.value_or(0.25f));
                std::vector<entt::entity> todo{ e.Handle };
                for (size_t i = 0; i < todo.size(); i++) {
                    if (auto* mr = e.Owner->Registry().try_get<MeshRendererComponent>(todo[i])) {
                        mr->FlashColor = c;
                        mr->FlashTime = mr->FlashDuration = t;
                    }
                    for (entt::entity child : e.Owner->GetChildren(todo[i])) todo.push_back(child);
                }
            },
            "look_at", [](const ScriptEntity& e, const glm::vec3& target, sol::optional<glm::vec3> up) {
                e.Get<TransformComponent>();
                const glm::vec3 d = target - glm::vec3(e.Owner->GetWorldMatrix(e.Handle)[3]);   // in world space
                if (glm::dot(d, d) < 1e-10f) return;
                const glm::vec3 dir = glm::normalize(d);
                glm::vec3 u = up ? glm::normalize(*up) : glm::vec3(0.0f, 1.0f, 0.0f);
                if (std::abs(glm::dot(dir, u)) > 0.999f) u = glm::vec3(0.0f, 0.0f, 1.0f);   // looking straight up/down
                e.Owner->SetWorldRotation(e.Handle, glm::quatLookAt(dir, u));
            },
            "particles", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<ParticlesRef> {
                if (e.Has<ParticleEmitterComponent>()) return ParticlesRef{ e };
                return sol::nullopt;
            }),
            // add_particles("Fire") or add_particles{ preset = "Fire", rate, burst, texture, additive, emitting }
            "add_particles", [this](const ScriptEntity& e, sol::object opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                ParticleEmitterComponent pe;
                if (opts.is<std::string>()) {
                    if (!ApplyParticlePreset(pe, opts.as<std::string>())) throw std::runtime_error("unknown particle preset");
                }
                else if (opts.is<sol::table>()) {
                    sol::table t = opts.as<sol::table>();
                    if (auto pr = t.get<sol::optional<std::string>>("preset"); pr && !ApplyParticlePreset(pe, *pr))
                        throw std::runtime_error("unknown particle preset \"" + *pr + "\"");
                    pe.Rate = t.get<sol::optional<float>>("rate").value_or(pe.Rate);
                    pe.Burst = t.get<sol::optional<int>>("burst").value_or(pe.Burst);
                    pe.Emitting = t.get<sol::optional<bool>>("emitting").value_or(pe.Emitting);
                    pe.Additive = t.get<sol::optional<bool>>("additive").value_or(pe.Additive);
                    if (auto tex = t.get<sol::optional<std::string>>("texture"))
                        pe.Texture = Resources.FindTexture ? Resources.FindTexture(*tex) : nullptr;
                }
                e.Owner->Registry().emplace_or_replace<ParticleEmitterComponent>(e.Handle, pe);
                return ParticlesRef{ e };
            },
            "animator", sol::readonly_property([](const ScriptEntity& e) -> sol::optional<AnimatorRef> {
                if (e.Has<AnimatorComponent>()) return AnimatorRef{ e };
                return sol::nullopt;
            }),
            "add_animator", [](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                AnimatorComponent a;
                if (opts) {
                    a.Clip = opts->get_or<std::string>("clip", a.Clip);
                    a.Speed = opts->get_or<float>("speed", a.Speed);
                    a.Loop = opts->get_or<bool>("loop", a.Loop);
                    a.BlendTime = opts->get_or<float>("blend", a.BlendTime);
                }
                e.Owner->Registry().emplace_or_replace<AnimatorComponent>(e.Handle, a);
                return AnimatorRef{ e };
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
                if (kind == "collider") return e.Has<ColliderComponent>();
                if (kind == "animator") return e.Has<AnimatorComponent>();
                if (kind == "particles") return e.Has<ParticleEmitterComponent>();
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
                else if (kind == "collider") reg.remove<ColliderComponent>(e.Handle);
                else if (kind == "animator") reg.remove<AnimatorComponent>(e.Handle);
                else if (kind == "particles") reg.remove<ParticleEmitterComponent>(e.Handle);
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
                    if (const auto am = opts->get<sol::optional<std::string>>("alpha_mode")) mr.Alpha = AlphaModeFromString(*am);
                    if (const auto op = opts->get<sol::optional<float>>("opacity")) {
                        mr.Opacity = std::clamp(*op, 0.0f, 1.0f);
                        if (mr.Opacity < 1.0f && mr.Alpha == AlphaMode::Opaque) mr.Alpha = AlphaMode::Blend;
                    }
                    mr.AlphaCutoff = opts->get<sol::optional<float>>("alpha_cutoff").value_or(mr.AlphaCutoff);
                    mr.CastShadows = opts->get<sol::optional<bool>>("cast_shadows").value_or(mr.CastShadows);
                    mr.Static = opts->get<sol::optional<bool>>("static").value_or(mr.Static);
                    mr.DoubleSided = opts->get<sol::optional<bool>>("double_sided").value_or(mr.DoubleSided);
                    mr.Specular = std::clamp(opts->get<sol::optional<float>>("specular").value_or(mr.Specular), 0.0f, 1.0f);
                    mr.Wind = std::max(opts->get<sol::optional<float>>("wind").value_or(mr.Wind), 0.0f);
                    // fit = metres: a model file is scaled and centred to that size (models come in any units)
                    if (const auto fit = opts->get<sol::optional<float>>("fit")) {
                        if (const auto* model = dynamic_cast<const Model*>(mr.Geometry.get())) {
                            mr.LocalOffset = model->GetGroundedMatrix(*fit);   // feet on the entity's origin
                            mr.UseModelMaterials = model->HasTexturedMaterials();
                            if (mr.UseModelMaterials && !opts->get<sol::optional<glm::vec3>>("color")) mr.Color = glm::vec3(1.0f);
                        }
                    }
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
                    else if (shape == "convex") rb.Shape = ColliderShape::ConvexHull;   // from the entity's mesh
                    else if (shape == "mesh") rb.Shape = ColliderShape::Mesh;           // exact, static/kinematic only
                    else if (shape == "capsule") rb.Shape = ColliderShape::Capsule;     // size = vec3(diameter, height, _)
                    else if (shape == "none") rb.Shape = ColliderShape::None;           // child colliders only
                    else if (shape != "box") throw std::runtime_error("unknown shape '" + shape + "' (box, sphere, capsule, convex, mesh, none)");
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
            // An extra shape for the rigid body above this entity (a compound collider)
            "add_collider", [this](const ScriptEntity& e, sol::optional<sol::table> opts) {
                if (!e.Valid()) throw std::runtime_error("entity no longer exists");
                ColliderComponent c;
                if (opts) {
                    const std::string shape = opts->get_or<std::string>("shape", "box");
                    if (shape == "sphere") c.Shape = ColliderShape::Sphere;
                    else if (shape == "capsule") c.Shape = ColliderShape::Capsule;
                    else if (shape == "convex") c.Shape = ColliderShape::ConvexHull;
                    else if (shape == "mesh") c.Shape = ColliderShape::Mesh;
                    else if (shape != "box") throw std::runtime_error("unknown collider shape '" + shape + "' (box, sphere, capsule, convex, mesh)");
                    sol::object size = (*opts)["size"];
                    if (size.is<glm::vec3>())  c.Size = size.as<glm::vec3>();
                    else if (size.is<float>()) c.Size = glm::vec3(size.as<float>());
                    c.Center = opts->get_or<glm::vec3>("center", c.Center);
                }
                e.Owner->Registry().emplace_or_replace<ColliderComponent>(e.Handle, c);
                // Rebuild the body it belongs to (the nearest ancestor with a RigidBody)
                int guard = 0;
                for (entt::entity p = e.Owner->GetParent(e.Handle); p != entt::null && guard++ < 256; p = e.Owner->GetParent(p))
                    if (e.Owner->Registry().all_of<RigidBodyComponent>(p)) { if (Physics) Physics->Invalidate(p); break; }
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
        // A one-shot effect: an entity with a preset emitter that bursts `count` particles and removes itself
        // when they are gone. scene.spawn_particles(pos, "Sparks") or (pos, { preset = "Sparks", count = 30 })
        scene.set_function("spawn_particles", [this](const glm::vec3& pos, sol::object opts) {
            std::string preset = "Sparks";
            int count = -1;
            if (opts.is<std::string>()) preset = opts.as<std::string>();
            else if (opts.is<sol::table>()) {
                sol::table t = opts.as<sol::table>();
                preset = t.get<sol::optional<std::string>>("preset").value_or(preset);
                count = t.get<sol::optional<int>>("count").value_or(count);
            }
            Entity e = ActiveScene->CreateEntity(preset + " burst");
            e.Get<TransformComponent>().Position = pos;
            auto& pe = e.Add<ParticleEmitterComponent>();
            if (!ApplyParticlePreset(pe, preset)) throw std::runtime_error("unknown particle preset \"" + preset + "\"");
            pe.Emitting = false;
            pe.Burst = count >= 0 ? count : (pe.Burst > 0 ? pe.Burst : 30);
            pe.DestroyWhenDone = true;
            e.Add<TransientComponent>();        // an effect: never saved
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
        scene.set_function("quit", [this]() { QuitRequested = true; });         // the app closes (editor: stops Play)
        // the last finished frame: { draw_calls, triangles, instances, culled }
        scene.set_function("render_stats", [this]() {
            const auto& c = RenderStats::LastFrame;
            return Lua->create_table_with("draw_calls", c.DrawCalls, "triangles", c.Triangles,
                "instances", c.Instances, "culled", c.Culled, "shadow_skipped", c.ShadowSkipped, "particles", c.Particles,
                "static_batches", c.StaticBatches, "static_merged", c.StaticMerged, "decals", c.Decals, "foliage", c.Foliage);
        });
        scene.set_function("raycast", [this](const glm::vec3& origin, const glm::vec3& dir, float maxDist,
            sol::optional<ScriptEntity> ignore) -> sol::object {
            RayHit hit;
            if (!Physics || !Physics->Raycast(origin, dir, maxDist, hit, ignore ? ignore->Handle : entt::null))
                return sol::make_object(*Lua, sol::lua_nil);
            return Lua->create_table_with("entity", Wrap(hit.Entity), "point", hit.Point,
                "normal", hit.Normal, "distance", hit.Distance);
        });

        // ---- ui: drawn over the game view this frame, in call order. x, y, w, h are 0..1 of the view
        sol::table ui = lua.create_named_table("ui");
        auto push = [this](ScriptUIItem item) { if (UI.size() < 1024) UI.push_back(std::move(item)); };
        auto style = [](const sol::optional<sol::table>& opts, ScriptUIItem& it) {
            if (!opts) return;
            it.Color = opts->get_or<glm::vec3>("color", it.Color);
            it.Alpha = opts->get_or<float>("alpha", it.Alpha);
            it.Rounding = opts->get_or<float>("rounding", it.Rounding);
            it.Size = opts->get_or<float>("size", it.Size);
            it.TextColor = opts->get_or<glm::vec3>("text_color", it.TextColor);
        };

        ui.set_function("text", [push, style](const std::string& text, float x, float y, sol::optional<sol::table> opts) {
            ScriptUIItem t;
            t.Text = text;
            t.X = x;
            t.Y = y;
            style(opts, t);
            if (opts) {
                const std::string align = opts->get_or<std::string>("align", "left");
                t.Align = align == "center" ? 1 : align == "right" ? 2 : 0;
            }
            push(std::move(t));
        });
        ui.set_function("rect", [push, style](float x, float y, float w, float h, sol::optional<sol::table> opts) {
            ScriptUIItem r;
            r.Kind = ScriptUIItem::Type::Rect;
            r.X = x; r.Y = y; r.W = w; r.H = h;
            style(opts, r);
            push(std::move(r));
        });
        // ---- option readers for the laid-out widgets
        auto optF = [](const sol::optional<sol::table>& t, const char* k, float d) { return t ? t->get<sol::optional<float>>(k).value_or(d) : d; };
        auto optB = [](const sol::optional<sol::table>& t, const char* k, bool d) { return t ? t->get<sol::optional<bool>>(k).value_or(d) : d; };
        auto optS = [](const sol::optional<sol::table>& t, const char* k, const std::string& d) {
            return t ? t->get<sol::optional<std::string>>(k).value_or(d) : d;
        };
        auto optV = [](const sol::optional<sol::table>& t, const char* k, glm::vec3& out) {
            if (!t) return false;
            auto v = t->get<sol::optional<glm::vec3>>(k);
            if (v) out = *v;
            return (bool)v;
        };
        auto tableAt = [](const sol::variadic_args& va, size_t i) -> sol::optional<sol::table> {
            if (i < va.size() && va[i].get_type() == sol::type::table) return va[i].as<sol::table>();
            return sol::nullopt;
        };
        auto isPlaced = [](const sol::variadic_args& va, size_t minArgs) {   // the older form: positions first
            return va.size() >= minArgs && va[0].get_type() == sol::type::number;
        };

        // A filled bar. Laid out: ui.bar(fraction [, opts]). Placed: ui.bar(x, y, w, h, fraction [, opts]):
        // the back over the whole rect, the colour over `fraction` of it (from the left)
        ui.set_function("bar", [this, push, style, optF, optS, optV, tableAt, isPlaced](sol::variadic_args va) {
            if (isPlaced(va, 5)) {
                const float x = va[0].as<float>(), y = va[1].as<float>(), w = va[2].as<float>(), h = va[3].as<float>();
                const float fraction = va[4].as<float>();
                const sol::optional<sol::table> opts = tableAt(va, 5);
                ScriptUIItem back;
                back.Kind = ScriptUIItem::Type::Rect;
                back.X = x; back.Y = y; back.W = w; back.H = h;
                back.Color = glm::vec3(0.1f);
                back.Alpha = 0.6f;
                if (opts) {
                    back.Color = opts->get_or<glm::vec3>("back", back.Color);
                    back.Rounding = opts->get_or<float>("rounding", back.Rounding);
                }
                ScriptUIItem fill = back;
                fill.Color = glm::vec3(0.3f, 0.85f, 0.4f);
                fill.Alpha = 1.0f;
                style(opts, fill);
                fill.W = w * std::clamp(fraction, 0.0f, 1.0f);
                push(std::move(back));
                if (fill.W > 0.0f) push(std::move(fill));
                return;
            }
            const float fraction = va.size() > 0 ? va[0].as<float>() : 0.0f;
            const sol::optional<sol::table> opts = tableAt(va, 1);
            glm::vec3 color(0.3f, 0.85f, 0.4f), back(0.1f);
            optV(opts, "color", color);
            optV(opts, "back", back);
            UIC.Bar(fraction, optF(opts, "w", 0.0f), optF(opts, "h", 0.0f), color, back, optS(opts, "text", ""));
        });
        // Laid out: ui.image(path [, opts]). Placed: ui.image(path, x, y, w [, h] [, opts])
        ui.set_function("image", [this, push, style, optF, optV, tableAt, isPlaced](const std::string& path, sol::variadic_args va) {
            if (isPlaced(va, 3)) {
                ScriptUIItem img;
                img.Kind = ScriptUIItem::Type::Image;
                img.Text = path;
                img.X = va[0].as<float>(); img.Y = va[1].as<float>(); img.W = va[2].as<float>();
                sol::optional<sol::table> opts;
                if (va.size() > 3) {
                    if (va[3].get_type() == sol::type::number) img.H = va[3].as<float>();
                    else opts = tableAt(va, 3);
                }
                if (!opts) opts = tableAt(va, 4);
                style(opts, img);
                push(std::move(img));
                return;
            }
            const sol::optional<sol::table> opts = tableAt(va, 0);
            glm::vec3 color(1.0f);
            optV(opts, "color", color);
            UIC.Image(path, optF(opts, "w", 64.0f), optF(opts, "h", 0.0f), color, optF(opts, "alpha", 1.0f));
        });
        // Laid out: ui.button(label [, opts]) -> clicked (on release). Placed: ui.button(label, x, y, w, h [, opts])
        // -> clicked (on press), drawn where it is told
        ui.set_function("button", [this, push, style, optF, optB, optS, optV, tableAt, isPlaced](const std::string& label, sol::variadic_args va) {
            if (isPlaced(va, 4)) {
                const float x = va[0].as<float>(), y = va[1].as<float>(), w = va[2].as<float>(), h = va[3].as<float>();
                ScriptUIItem b;
                b.Kind = ScriptUIItem::Type::Button;
                b.Text = label;
                b.X = x; b.Y = y; b.W = w; b.H = h;
                b.Color = glm::vec3(0.16f, 0.2f, 0.3f);
                b.Alpha = 0.92f;
                b.Rounding = 10.0f;
                b.Size = 28.0f;
                style(tableAt(va, 4), b);
                b.Hovered = PtrInside && PtrX >= x && PtrX <= x + w && PtrY >= y && PtrY <= y + h;
                b.Pressed = b.Hovered && PtrDown;
                const bool clicked = b.Hovered && PtrClicked;
                push(std::move(b));
                UIC.AddBlocker(x, y, w, h);          // ui.wants_mouse() over it
                return clicked;
            }
            const sol::optional<sol::table> opts = tableAt(va, 0);
            UIButtonOptions o;
            o.Id = optS(opts, "id", "");
            o.W = optF(opts, "w", 0.0f);
            o.H = optF(opts, "h", 0.0f);
            o.Fill = optB(opts, "fill", false);
            o.Size = optF(opts, "size", 0.0f);
            o.HasColor = optV(opts, "color", o.Color);
            o.Disabled = optB(opts, "disabled", false);
            o.Selected = optB(opts, "selected", false);
            o.Tooltip = optS(opts, "tooltip", "");
            return UIC.Button(label, o);
        });
        ui.set_function("mouse", [this]() { return std::make_tuple(PtrX, PtrY, PtrInside); });

        // ---- laid-out UI: panels, rows, scroll regions and the widgets inside them (Runtime/UILayout.h)
        ui.set_function("begin_panel", [this, optF, optB, optS, optV](const std::string& id, sol::optional<sol::table> opts) {
            static const std::unordered_map<std::string, glm::vec2> anchors = {
                { "center", { 0.5f, 0.5f } }, { "top", { 0.5f, 0.0f } }, { "bottom", { 0.5f, 1.0f } },
                { "left", { 0.0f, 0.5f } }, { "right", { 1.0f, 0.5f } }, { "topleft", { 0.0f, 0.0f } },
                { "topright", { 1.0f, 0.0f } }, { "bottomleft", { 0.0f, 1.0f } }, { "bottomright", { 1.0f, 1.0f } } };
            UIPanelOptions o;
            o.X = optF(opts, "x", 0.5f);
            o.Y = optF(opts, "y", 0.5f);
            o.W = optF(opts, "w", 400.0f);
            o.H = optF(opts, "h", 0.0f);
            const std::string anchor = optS(opts, "anchor", "center");
            const auto it = anchors.find(anchor);
            if (it == anchors.end()) throw std::runtime_error("ui.begin_panel: unknown anchor '" + anchor + "'");
            o.Pivot = it->second;
            o.Title = optS(opts, "title", "");
            o.Close = optB(opts, "close", false);
            o.Padding = optF(opts, "padding", -1.0f);
            o.HasColor = optV(opts, "color", o.Color);
            o.Alpha = optF(opts, "alpha", -1.0f);
            o.Image = optS(opts, "image", "");
            o.Slice = optF(opts, "slice", -1.0f);
            return UIC.BeginPanel(id, o);
        });
        ui.set_function("end_panel", [this]() { UIC.EndPanel(); });
        ui.set_function("begin_row", [this](sol::optional<float> spacing) { UIC.BeginRow(spacing.value_or(-1.0f)); });
        ui.set_function("end_row", [this]() { UIC.EndRow(); });
        ui.set_function("begin_scroll", [this](const std::string& id, float height) { UIC.BeginScroll(id, height); });
        ui.set_function("end_scroll", [this]() { UIC.EndScroll(); });
        ui.set_function("begin_column", [this](sol::optional<float> width) { UIC.BeginColumn(width.value_or(0.0f)); });
        ui.set_function("end_column", [this]() { UIC.EndColumn(); });
        ui.set_function("label", [this, optF, optB, optS, optV](const std::string& text, sol::optional<sol::table> opts) {
            UITextOptions o;
            o.Size = optF(opts, "size", 0.0f);
            o.HasColor = optV(opts, "color", o.Color);
            o.Alpha = optF(opts, "alpha", 1.0f);
            const std::string align = optS(opts, "align", "left");
            o.Align = align == "center" ? 1 : align == "right" ? 2 : 0;
            o.Bold = optB(opts, "bold", false);
            o.Muted = optB(opts, "muted", false);
            o.Wrap = optB(opts, "wrap", true);
            o.Shadow = optB(opts, "shadow", false);
            o.Width = optF(opts, "width", 0.0f);
            UIC.Label(text, o);
        });
        ui.set_function("slot", [this, optF, optB, optS, optV](const std::string& id, sol::optional<sol::table> opts) {
            UISlotOptions o;
            o.Icon = optS(opts, "icon", "");
            o.Count = (int)optF(opts, "count", 0.0f);
            o.Size = optF(opts, "size", 0.0f);
            o.Tooltip = optS(opts, "tooltip", "");
            const sol::object drag = opts ? opts->get<sol::object>("drag") : sol::object();
            o.Draggable = drag.valid() && drag.get_type() != sol::type::lua_nil && drag.get_type() != sol::type::none;
            o.Accept = optB(opts, "accept", true);
            o.Highlight = optB(opts, "highlight", false);
            o.HasColor = optV(opts, "color", o.Color);
            o.IconAlpha = optF(opts, "icon_alpha", 1.0f);
            const UISlotResult r = UIC.Slot(id, o);
            if (r.DragStarted) DragPayload = drag;
            sol::object dropped = sol::make_object(*Lua, sol::lua_nil);
            if (r.Dropped) dropped = DragPayload.valid() ? DragPayload : sol::make_object(*Lua, true);
            return std::make_tuple(r.Clicked, dropped, r.RightClicked, r.Hovered);
        });
        ui.set_function("tabs", [this](const std::string& id, sol::table labels, int current) {
            std::vector<std::string> list;
            for (size_t i = 1; i <= labels.size(); i++) list.push_back(labels.get<std::string>(i));
            return UIC.Tabs(id, list, current - 1) + 1;
        });
        ui.set_function("space", [this](sol::optional<float> h) { UIC.Space(h.value_or(UIC.Theme.Spacing)); });
        ui.set_function("separator", [this]() { UIC.Separator(); });
        ui.set_function("tooltip", [this](const std::string& text) { UIC.Tooltip(text); });
        ui.set_function("wants_mouse", [this]() { return UIC.WantsMouse(); });
        ui.set_function("dragging", [this]() -> sol::object {
            return UIC.Dragging() && DragPayload.valid() ? DragPayload : sol::make_object(*Lua, sol::lua_nil);
        });
        ui.set_function("last_rect", [this]() {
            float x, y, w, h;
            UIC.LastRect(x, y, w, h);
            return std::make_tuple(x, y, w, h);
        });
        ui.set_function("text_width", [this](const std::string& text, sol::optional<float> size, sol::optional<bool> bold) {
            return UIC.TextWidth(text, size.value_or(UIC.Theme.TextSize), bold.value_or(false));
        });
        ui.set_function("simulate_pointer", [this](sol::optional<float> x, sol::optional<float> y, sol::optional<bool> down,
            sol::optional<float> wheel, sol::optional<bool> right) {
            if (!x) { SimPointer = false; return; }
            SimPointer = true;
            Sim = UIPointerState{ *x, y.value_or(0.0f), true, down.value_or(false), right.value_or(false), wheel.value_or(0.0f) };
        });
        ui.set_function("theme", [this](sol::table t) {
            UITheme& th = UIC.Theme;
            auto f = [&](const char* k, float& v) { v = t.get<sol::optional<float>>(k).value_or(v); };
            auto c = [&](const char* k, glm::vec3& v) { v = t.get<sol::optional<glm::vec3>>(k).value_or(v); };
            auto s = [&](const char* k, std::string& v) { v = t.get<sol::optional<std::string>>(k).value_or(v); };
            c("panel_color", th.PanelColor); f("panel_alpha", th.PanelAlpha); s("panel_image", th.PanelImage); f("panel_slice", th.PanelSlice);
            c("border_color", th.BorderColor); f("border_alpha", th.BorderAlpha);
            c("title_color", th.TitleColor); f("title_size", th.TitleSize);
            c("text_color", th.TextColor); f("text_size", th.TextSize); c("muted_color", th.MutedColor);
            c("button_color", th.ButtonColor); c("accent_color", th.AccentColor);
            f("button_text_size", th.ButtonTextSize); f("button_height", th.ButtonHeight);
            f("padding", th.Padding); f("spacing", th.Spacing); f("rounding", th.Rounding);
            c("slot_color", th.SlotColor); f("slot_size", th.SlotSize); s("slot_image", th.SlotImage); f("slot_slice", th.SlotSlice);
            f("tooltip_width", th.TooltipWidth);
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

        // ---- the Animator view handed out by e.animator
        // ---- the Particle Emitter view handed out by e.particles
        lua.new_usertype<ParticlesRef>("ParticleEmitter", sol::no_constructor,
            "emitting", sol::property(
                [](const ParticlesRef& p) { return p.E.Get<ParticleEmitterComponent>().Emitting; },
                [](const ParticlesRef& p, bool v) { p.E.Get<ParticleEmitterComponent>().Emitting = v; }),
            "rate", sol::property(
                [](const ParticlesRef& p) { return p.E.Get<ParticleEmitterComponent>().Rate; },
                [](const ParticlesRef& p, float v) { p.E.Get<ParticleEmitterComponent>().Rate = std::max(v, 0.0f); }),
            "emission", sol::property(
                [](const ParticlesRef& p) { return p.E.Get<ParticleEmitterComponent>().Emission; },
                [](const ParticlesRef& p, float v) { p.E.Get<ParticleEmitterComponent>().Emission = std::max(v, 0.0f); }),
            "gravity_scale", sol::property(
                [](const ParticlesRef& p) { return p.E.Get<ParticleEmitterComponent>().GravityScale; },
                [](const ParticlesRef& p, float v) { p.E.Get<ParticleEmitterComponent>().GravityScale = v; }),
            // colours: rgb (sRGB-style, like the pickers) + a separate alpha
            "start_color", sol::property(
                [](const ParticlesRef& p) { return glm::vec3(p.E.Get<ParticleEmitterComponent>().StartColor); },
                [](const ParticlesRef& p, const glm::vec3& v) { auto& c = p.E.Get<ParticleEmitterComponent>().StartColor; c = glm::vec4(v, c.a); }),
            "end_color", sol::property(
                [](const ParticlesRef& p) { return glm::vec3(p.E.Get<ParticleEmitterComponent>().EndColor); },
                [](const ParticlesRef& p, const glm::vec3& v) { auto& c = p.E.Get<ParticleEmitterComponent>().EndColor; c = glm::vec4(v, c.a); }),
            "count", sol::readonly_property([](const ParticlesRef& p) { return (int)p.E.Get<ParticleEmitterComponent>().Particles.size(); }),
            "burst", [](const ParticlesRef& p, sol::optional<int> n) {
                auto& pe = p.E.Get<ParticleEmitterComponent>();
                pe.PendingBurst += std::max(0, n.value_or(pe.Burst > 0 ? pe.Burst : 20));
            },
            "clear", [](const ParticlesRef& p) { p.E.Get<ParticleEmitterComponent>().Particles.clear(); },
            "preset", [](const ParticlesRef& p, const std::string& name) {
                if (!ApplyParticlePreset(p.E.Get<ParticleEmitterComponent>(), name))
                    throw std::runtime_error("unknown particle preset \"" + name + "\"");
            });
        lua.new_usertype<AnimatorRef>("Animator", sol::no_constructor,
            "clip", sol::property(                                // setting it cross-fades to that clip
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().Clip; },
                [](const AnimatorRef& a, const std::string& v) { a.E.Get<AnimatorComponent>().Clip = v; }),
            "speed", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().Speed; },
                [](const AnimatorRef& a, float v) { a.E.Get<AnimatorComponent>().Speed = v; }),
            "loop", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().Loop; },
                [](const AnimatorRef& a, bool v) { a.E.Get<AnimatorComponent>().Loop = v; }),
            "playing", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().Playing; },
                [](const AnimatorRef& a, bool v) { a.E.Get<AnimatorComponent>().Playing = v; }),
            "time", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().Time; },
                [](const AnimatorRef& a, float v) { a.E.Get<AnimatorComponent>().Time = v; }),
            // root motion: the root bone's sideways movement moves the entity (root_bone "" = automatic)
            "root_motion", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().RootMotion; },
                [](const AnimatorRef& a, bool v) { a.E.Get<AnimatorComponent>().RootMotion = v; }),
            "root_bone", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().RootBone; },
                [](const AnimatorRef& a, const std::string& v) { a.E.Get<AnimatorComponent>().RootBone = v; }),
            "blend_time", sol::property(
                [](const AnimatorRef& a) { return a.E.Get<AnimatorComponent>().BlendTime; },
                [](const AnimatorRef& a, float v) { a.E.Get<AnimatorComponent>().BlendTime = std::max(v, 0.0f); }),
            // play(name [, blend seconds]): cross-fade to a clip and make sure it runs
            "play", [](const AnimatorRef& a, const std::string& clip, sol::optional<float> blend) {
                auto& an = a.E.Get<AnimatorComponent>();
                if (blend) an.BlendTime = std::max(*blend, 0.0f);
                an.Clip = clip;
                an.Playing = true;
            },
            "stop", [](const AnimatorRef& a) { a.E.Get<AnimatorComponent>().Playing = false; },
            // ---- IK (1-based): set_ik(n, { type = "look"|"limb", bone, target = vec3 or entity, offset = vec3,
            //      weight, max_angle (degrees), chain, pole = vec3 (entity space) }) creates or changes one
            "set_ik", [](const AnimatorRef& a, int n, sol::table opts) {
                auto& iks = a.E.Get<AnimatorComponent>().IK;
                if (n < 1 || n > 8) throw std::runtime_error("IK number must be 1..8");
                if ((int)iks.size() < n) iks.resize(n);
                AnimIK& ik = iks[n - 1];
                if (const auto t = opts.get<sol::optional<std::string>>("type")) {
                    if (*t == "look") ik.Type = IKType::Look;
                    else if (*t == "limb") ik.Type = IKType::Limb;
                    else throw std::runtime_error("IK type must be \"look\" or \"limb\"");
                }
                ik.Bone = opts.get<sol::optional<std::string>>("bone").value_or(ik.Bone);
                sol::object target = opts["target"];
                if (target.is<ScriptEntity>()) {
                    const ScriptEntity t = target.as<ScriptEntity>();
                    if (!t.Valid() || !t.Has<IDComponent>()) throw std::runtime_error("IK target entity no longer exists");
                    ik.TargetID = t.Get<IDComponent>().ID;
                    ik.Target = opts.get<sol::optional<glm::vec3>>("offset").value_or(glm::vec3(0.0f));
                }
                else if (target.is<glm::vec3>()) {
                    ik.TargetID = 0;
                    ik.Target = target.as<glm::vec3>();
                }
                if (const auto w = opts.get<sol::optional<float>>("weight")) ik.Weight = std::clamp(*w, 0.0f, 1.0f);
                ik.MaxAngle = opts.get<sol::optional<float>>("max_angle").value_or(ik.MaxAngle);
                ik.Chain = std::max(1, opts.get<sol::optional<int>>("chain").value_or(ik.Chain));
                ik.Pole = opts.get<sol::optional<glm::vec3>>("pole").value_or(ik.Pole);
            },
            "ik_weight", [](const AnimatorRef& a, int n, float w) {
                auto& iks = a.E.Get<AnimatorComponent>().IK;
                if (n < 1 || n > (int)iks.size()) throw std::runtime_error("no IK " + std::to_string(n));
                iks[n - 1].Weight = std::clamp(w, 0.0f, 1.0f);
            },
            "remove_ik", [](const AnimatorRef& a, int n) {
                auto& iks = a.E.Get<AnimatorComponent>().IK;
                if (n >= 1 && n <= (int)iks.size()) iks.erase(iks.begin() + (n - 1));
            },
            "ik_count", [](const AnimatorRef& a) { return (int)a.E.Get<AnimatorComponent>().IK.size(); },
            "ik", [this](const AnimatorRef& a, int n) -> sol::object {
                const auto& iks = a.E.Get<AnimatorComponent>().IK;
                if (n < 1 || n > (int)iks.size()) return sol::lua_nil;
                const AnimIK& ik = iks[n - 1];
                sol::table t = Lua->create_table_with("type", ik.Type == IKType::Limb ? "limb" : "look", "bone", ik.Bone,
                    "weight", ik.Weight, "max_angle", ik.MaxAngle, "chain", ik.Chain, "pole", ik.Pole);
                // target: always the world point it aims at now; entity + offset when it follows an entity
                if (ik.TargetID != 0) {
                    const entt::entity te = a.E.Owner->FindByID(ik.TargetID);
                    t["offset"] = ik.Target;
                    if (te != entt::null) {
                        t["entity"] = MakeEntity(te);
                        t["target"] = glm::vec3(a.E.Owner->GetWorldMatrix(te)[3]) + ik.Target;
                    }
                }
                else t["target"] = ik.Target;
                return t;
            },
            // the world direction a bone faces (at rest: where the entity faces); nil if no such bone
            "bone_forward", [](const AnimatorRef& a, const std::string& bone) -> sol::optional<glm::vec3> {
                a.E.Get<AnimatorComponent>();
                const glm::vec3 d = a.E.Owner->BoneForward(a.E.Handle, bone);
                if (std::isnan(d.x)) return sol::nullopt;
                return d;
            },
            // where a bone is now, in world space (after layers and IK); nil if there is no such bone
            "bone_position", [](const AnimatorRef& a, const std::string& bone) -> sol::optional<glm::vec3> {
                a.E.Get<AnimatorComponent>();
                const glm::vec3 p = a.E.Owner->BonePosition(a.E.Handle, bone);
                if (std::isnan(p.x)) return sol::nullopt;
                return p;
            },
            // ---- events: add_event(clip, time, name), clear_events([clip]), events() -> { {clip, time, name}, ... }
            "add_event", [](const AnimatorRef& a, const std::string& clip, float time, const std::string& name) {
                a.E.Get<AnimatorComponent>().Events.push_back({ clip, time, name });
            },
            "clear_events", [](const AnimatorRef& a, sol::optional<std::string> clip) {
                auto& evs = a.E.Get<AnimatorComponent>().Events;
                if (!clip) { evs.clear(); return; }
                evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const AnimEvent& e) { return e.Clip == *clip; }), evs.end());
            },
            "events", [this](const AnimatorRef& a) {
                sol::table out = Lua->create_table();
                int n = 1;
                for (const auto& ev : a.E.Get<AnimatorComponent>().Events)
                    out[n++] = Lua->create_table_with("clip", ev.Clip, "time", ev.Time, "name", ev.Name);
                return out;
            },
            // ---- layers (1-based): set_layer(n, { clip, mask, weight, speed, loop }) creates or changes one
            "set_layer", [](const AnimatorRef& a, int n, sol::table opts) {
                auto& layers = a.E.Get<AnimatorComponent>().Layers;
                if (n < 1 || n > 8) throw std::runtime_error("layer number must be 1..8");
                if ((int)layers.size() < n) layers.resize(n);
                AnimLayer& L = layers[n - 1];
                if (const auto c = opts.get<sol::optional<std::string>>("clip")) { if (*c != L.Clip) L.Time = 0.0f; L.Clip = *c; }
                L.Mask = opts.get<sol::optional<std::string>>("mask").value_or(L.Mask);
                if (const auto w = opts.get<sol::optional<float>>("weight")) { L.Weight = std::clamp(*w, 0.0f, 1.0f); L.TargetWeight = -1.0f; }
                L.Speed = opts.get<sol::optional<float>>("speed").value_or(L.Speed);
                L.Loop = opts.get<sol::optional<bool>>("loop").value_or(L.Loop);
                L.Additive = opts.get<sol::optional<bool>>("additive").value_or(L.Additive);
            },
            // layer_weight(n, weight [, seconds]): set it now, or fade to it over that many seconds
            "layer_weight", [](const AnimatorRef& a, int n, float w, sol::optional<float> seconds) {
                auto& layers = a.E.Get<AnimatorComponent>().Layers;
                if (n < 1 || n > (int)layers.size()) throw std::runtime_error("no layer " + std::to_string(n));
                AnimLayer& L = layers[n - 1];
                w = std::clamp(w, 0.0f, 1.0f);
                if (seconds && *seconds > 0.0f) { L.TargetWeight = w; L.FadeRate = std::abs(w - L.Weight) / *seconds; }
                else { L.Weight = w; L.TargetWeight = -1.0f; }
            },
            "layer", [this](const AnimatorRef& a, int n) -> sol::object {
                const auto& layers = a.E.Get<AnimatorComponent>().Layers;
                if (n < 1 || n > (int)layers.size()) return sol::lua_nil;
                const AnimLayer& L = layers[n - 1];
                return sol::make_object(*Lua, Lua->create_table_with("clip", L.Clip, "mask", L.Mask, "weight", L.Weight,
                    "speed", L.Speed, "loop", L.Loop, "additive", L.Additive, "time", L.Time));
            },
            "remove_layer", [](const AnimatorRef& a, int n) {
                auto& layers = a.E.Get<AnimatorComponent>().Layers;
                if (n >= 1 && n <= (int)layers.size()) layers.erase(layers.begin() + (n - 1));
            },
            "layer_count", [](const AnimatorRef& a) { return (int)a.E.Get<AnimatorComponent>().Layers.size(); },
            // the skeleton's bone (node) names, for layer masks
            "bones", [this](const AnimatorRef& a) {
                sol::table out = Lua->create_table();
                const auto* mr = a.E.Owner->Registry().try_get<MeshRendererComponent>(a.E.Handle);
                const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
                if (model) { int n = 1; for (const auto& name : model->NodeNames()) out[n++] = name; }
                return out;
            },
            // clip length in seconds (0 if unknown); no argument: the current clip
            "duration", [](const AnimatorRef& a, sol::optional<std::string> clip) {
                const auto& an = a.E.Get<AnimatorComponent>();
                const auto* mr = a.E.Owner->Registry().try_get<MeshRendererComponent>(a.E.Handle);
                const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
                return model ? model->AnimationDuration(model->FindAnimation(clip.value_or(an.Clip))) : 0.0f;
            },
            // the clip names the entity's model has
            "clips", [this](const AnimatorRef& a) {
                a.E.Get<AnimatorComponent>();
                sol::table out = Lua->create_table();
                const auto* mr = a.E.Owner->Registry().try_get<MeshRendererComponent>(a.E.Handle);
                const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
                if (model) { int n = 1; for (const auto& name : model->AnimationNames()) out[n++] = name; }
                return out;
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

        BindNavigation();                // nav.* and scene.screen_ray / pick

        // ---- shared: one table every script sees (globals are per entity), for game-wide state
        lua["shared"] = lua.create_table();
        lua["game"] = lua.create_table();          // saved with the game, kept across level changes
        BindSaves();

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

    // ---------------------------------------------------------------- navigation and picking
    bool ScriptEngine::Impl::EnsureNav() {
        if (!Nav || !Physics) return false;
        if (Nav->IsBaked()) return true;
        if (NavTried) return false;          // a lazy bake failed already: nav.bake() tries again
        NavTried = true;
        return Nav->Bake(*Physics, NavConfig);
    }

    // A ray from the game camera (the primary CameraComponent, as GameSession::UpdateGameCamera picks it)
    // through the point x, y (0..1 of the view, top left origin)
    bool ScriptEngine::Impl::ScreenRay(float x, float y, glm::vec3& origin, glm::vec3& dir) const {
        auto& reg = ActiveScene->Registry();
        entt::entity chosen = entt::null;
        for (auto [e, c] : reg.view<CameraComponent>().each()) {
            if (chosen == entt::null) chosen = e;
            if (c.Primary) { chosen = e; break; }
        }
        if (chosen == entt::null) return false;
        glm::vec3 p, scale;
        glm::quat q;
        Scene::Decompose(ActiveScene->GetWorldMatrix(chosen), p, q, scale);
        const float t = std::tan(glm::radians(reg.get<CameraComponent>(chosen).FOV) * 0.5f);
        origin = p;
        dir = glm::normalize(q * glm::vec3((2.0f * x - 1.0f) * t * ViewAspect, (1.0f - 2.0f * y) * t, -1.0f));
        return true;
    }

    void ScriptEngine::Impl::BindNavigation() {
        sol::state& lua = *Lua;
        sol::table scene = lua["scene"];
        scene.set_function("screen_ray", [this](float x, float y) -> std::tuple<sol::object, sol::object> {
            glm::vec3 o, d;
            if (!ScreenRay(x, y, o, d)) return { sol::lua_nil, sol::lua_nil };
            return { sol::make_object(*Lua, o), sol::make_object(*Lua, d) };
        });
        // Where a world point is on the view (the inverse of screen_ray): x, y (0..1, top left origin), whether it
        // is in front of the camera and inside the view, and its depth (metres along the view direction).
        // Behind the camera x, y are nil. Health bars and floating text follow units with it.
        scene.set_function("world_to_screen", [this](const glm::vec3& world) -> std::tuple<sol::object, sol::object, bool, float> {
            auto& reg = ActiveScene->Registry();
            entt::entity chosen = entt::null;
            for (auto [e, c] : reg.view<CameraComponent>().each()) {
                if (chosen == entt::null) chosen = e;
                if (c.Primary) { chosen = e; break; }
            }
            const sol::object none = sol::make_object(*Lua, sol::lua_nil);
            if (chosen == entt::null) return { none, none, false, 0.0f };
            glm::vec3 p, scale;
            glm::quat q;
            Scene::Decompose(ActiveScene->GetWorldMatrix(chosen), p, q, scale);
            const glm::vec3 local = glm::inverse(q) * (world - p);       // camera space: looking along -Z
            const float depth = -local.z;
            if (depth <= 1e-4f) return { none, none, false, depth };
            const float t = std::tan(glm::radians(reg.get<CameraComponent>(chosen).FOV) * 0.5f);
            const float x = (local.x / depth) / (t * ViewAspect) * 0.5f + 0.5f;
            const float y = 0.5f - (local.y / depth) / t * 0.5f;
            const bool inside = x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
            return { sol::make_object(*Lua, x), sol::make_object(*Lua, y), inside, depth };
        });
        // ---- decals: shapes projected onto the ground for this frame only (like ui.*)
        sol::table decal = lua.create_named_table("decal");
        decal.set_function("draw", [this](const glm::vec3& pos, sol::optional<sol::table> opts) {
            if (ActiveScene->Decals.size() >= 4096) throw std::runtime_error("decal.draw: more than 4096 decals this frame");
            Decal d;
            d.Position = pos;
            if (opts) {
                const sol::table& t = *opts;
                const sol::object size = t["size"];
                if (size.is<glm::vec3>()) d.Size = size.as<glm::vec3>();
                else if (size.get_type() == sol::type::number) { const float s = size.as<float>(); d.Size = glm::vec3(s, 1.0f, s); }
                d.Size.y = t.get<sol::optional<float>>("height").value_or(d.Size.y);
                const std::string shape = t.get<sol::optional<std::string>>("shape").value_or("circle");
                if (shape == "circle") d.Kind = Decal::Shape::Circle;
                else if (shape == "ring") { d.Kind = Decal::Shape::Circle; d.Thickness = 0.15f; }
                else if (shape == "square") d.Kind = Decal::Shape::Square;
                else if (shape == "texture") d.Kind = Decal::Shape::Texture;
                else throw std::runtime_error("decal.draw: shape is \"circle\", \"ring\", \"square\" or \"texture\"");
                d.Color = t.get<sol::optional<glm::vec3>>("color").value_or(d.Color);
                d.Alpha = t.get<sol::optional<float>>("alpha").value_or(d.Alpha);
                d.Yaw = t.get<sol::optional<float>>("rotation").value_or(d.Yaw);
                d.Thickness = t.get<sol::optional<float>>("thickness").value_or(d.Thickness);
                d.Fill = t.get<sol::optional<float>>("fill").value_or(d.Fill);
                d.Softness = t.get<sol::optional<float>>("softness").value_or(d.Softness);
                d.Emission = t.get<sol::optional<float>>("emission").value_or(d.Emission);
                if (auto tex = t.get<sol::optional<std::string>>("texture")) {
                    std::string name = *tex;
                    if (name.rfind("texture:", 0) != 0 && name != "checker") name = "texture:" + name;
                    d.Texture = Resources.FindTexture ? Resources.FindTexture(name) : nullptr;
                    if (!d.Texture) throw std::runtime_error("decal.draw: can't load texture " + *tex);
                    d.Kind = Decal::Shape::Texture;
                }
            }
            ActiveScene->Decals.push_back(std::move(d));
        });
        decal.set_function("count", [this]() { return ActiveScene->Decals.size(); });

        // Grass (FoliageComponent): bodies press it by themselves; scripts press it where something without a
        // body walks (a character driven by its transform)
        sol::table foliage = lua.create_named_table("foliage");
        foliage.set_function("push", [this](const glm::vec3& pos, sol::optional<float> radius, sol::optional<float> strength) {
            if (ActiveScene->FoliagePushes.size() >= 256) return;     // more than enough for a frame
            ActiveScene->FoliagePushes.push_back({ pos, std::max(radius.value_or(0.5f), 0.01f), std::clamp(strength.value_or(1.0f), 0.0f, 2.0f) });
        });

        scene.set_function("pick", [this](float x, float y, sol::optional<float> maxDist) -> sol::object {
            glm::vec3 o, d;
            RayHit hit;
            if (!Physics || !ScreenRay(x, y, o, d) || !Physics->Raycast(o, d, maxDist.value_or(500.0f), hit))
                return sol::make_object(*Lua, sol::lua_nil);
            return Lua->create_table_with("entity", Wrap(hit.Entity), "point", hit.Point,
                "normal", hit.Normal, "distance", hit.Distance);
        });

        sol::table nav = lua.create_named_table("nav");
        auto nilOr = [this](int cell) { return cell >= 0 ? sol::make_object(*Lua, cell) : sol::make_object(*Lua, sol::lua_nil); };
        auto checkCell = [this](int cell) {
            if (cell < 0 || cell >= Nav->CellCount()) throw std::runtime_error("invalid nav cell " + std::to_string(cell));
            return cell;
        };

        nav.set_function("bake", [this](sol::optional<sol::table> opts) {
            if (!Nav || !Physics) return 0;
            NavSettings s;
            if (opts) {
                const sol::table& t = *opts;
                s.CellSize = t.get<sol::optional<float>>("cell_size").value_or(s.CellSize);
                s.AgentRadius = t.get<sol::optional<float>>("agent_radius").value_or(s.AgentRadius);
                s.AgentHeight = t.get<sol::optional<float>>("agent_height").value_or(s.AgentHeight);
                s.MaxStep = t.get<sol::optional<float>>("max_step").value_or(s.MaxStep);
                s.MaxSlope = t.get<sol::optional<float>>("max_slope").value_or(s.MaxSlope);
                auto mn = t.get<sol::optional<glm::vec3>>("min");
                auto mx = t.get<sol::optional<glm::vec3>>("max");
                if (mn && mx) { s.AutoBounds = false; s.BoundsMin = *mn; s.BoundsMax = *mx; }
            }
            NavConfig = s;
            NavTried = true;
            return Nav->Bake(*Physics, s) ? Nav->CellCount() : 0;
        });
        nav.set_function("baked", [this]() { return Nav && Nav->IsBaked(); });
        nav.set_function("cell_size", [this]() { return EnsureNav() ? Nav->Settings().CellSize : NavConfig.CellSize; });

        nav.set_function("find_path", [this](const glm::vec3& from, const glm::vec3& to, sol::optional<sol::table> opts) -> sol::object {
            if (!EnsureNav()) return sol::make_object(*Lua, sol::lua_nil);
            NavPathOptions o;
            if (opts) {
                const sol::table& t = *opts;
                o.Grid = t.get<sol::optional<bool>>("grid").value_or(o.Grid);
                o.Diagonal = t.get<sol::optional<float>>("diagonal").value_or(o.Diagonal);
                o.Closest = t.get<sol::optional<bool>>("closest").value_or(o.Closest);
                o.MaxCost = t.get<sol::optional<float>>("max_cost").value_or(o.MaxCost);
            }
            NavPath path;
            if (!Nav->FindPath(from, to, o, path)) return sol::make_object(*Lua, sol::lua_nil);
            sol::table points = Lua->create_table(), cells = Lua->create_table();
            for (size_t i = 0; i < path.Points.size(); i++) points[i + 1] = path.Points[i];
            for (size_t i = 0; i < path.Cells.size(); i++) cells[i + 1] = path.Cells[i];
            sol::table out = Lua->create_table_with("points", points, "cells", cells,
                "cost", path.Cost, "complete", path.Complete);
            if (o.Grid) {
                sol::table costs = Lua->create_table();
                for (size_t i = 0; i < path.Costs.size(); i++) costs[i + 1] = path.Costs[i];
                out["costs"] = costs;
            }
            return out;
        });

        nav.set_function("reachable", [this](sol::object from, float budget, sol::optional<sol::table> opts) {
            sol::table out = Lua->create_table();
            if (!EnsureNav()) return out;
            int start = -1;
            if (from.is<int>()) start = from.as<int>();
            else if (from.is<glm::vec3>()) {
                const glm::vec3 p = from.as<glm::vec3>();
                start = Nav->CellAt(p);
                if (start < 0) start = Nav->Nearest(p, Nav->Settings().CellSize * 1.5f, false);
            }
            else throw std::runtime_error("nav.reachable: from must be a vec3 or a cell");
            const float diag = opts ? opts->get<sol::optional<float>>("diagonal").value_or(1.41421356f) : 1.41421356f;
            std::vector<NavReach> reach;
            Nav->Reachable(start, budget, diag, reach);
            for (size_t i = 0; i < reach.size(); i++) {
                sol::table r = Lua->create_table_with("cell", reach[i].Cell, "cost", reach[i].Cost,
                    "position", Nav->CellPosition(reach[i].Cell));
                if (reach[i].Previous >= 0) r["previous"] = reach[i].Previous;
                out[i + 1] = r;
            }
            return out;
        });

        nav.set_function("cell_at", [this, nilOr](const glm::vec3& p) {
            return nilOr(EnsureNav() ? Nav->CellAt(p) : -1);
        });
        nav.set_function("nearest", [this, nilOr](const glm::vec3& p, sol::optional<float> maxDist, sol::optional<bool> free) {
            return nilOr(EnsureNav() ? Nav->Nearest(p, maxDist.value_or(2.0f), free.value_or(true)) : -1);
        });
        nav.set_function("cell_position", [this, checkCell](int cell) {
            if (!EnsureNav()) throw std::runtime_error("nav: no grid");
            return Nav->CellPosition(checkCell(cell));
        });
        nav.set_function("height", [this](const glm::vec3& p) -> sol::object {
            const int c = EnsureNav() ? Nav->CellAt(p) : -1;
            return c >= 0 ? sol::make_object(*Lua, Nav->CellPosition(c).y) : sol::make_object(*Lua, sol::lua_nil);
        });
        nav.set_function("line_clear", [this](const glm::vec3& a, const glm::vec3& b) {
            return EnsureNav() && Nav->LineClear(a, b);
        });
        nav.set_function("set_blocked", [this, checkCell](int cell, sol::optional<bool> blocked) {
            if (EnsureNav()) Nav->SetBlocked(checkCell(cell), blocked.value_or(true));
        });
        nav.set_function("is_blocked", [this](int cell) { return EnsureNav() && Nav->IsBlocked(cell); });
        nav.set_function("clear_blocked", [this]() { if (EnsureNav()) Nav->ClearBlocked(); });

        // ---- fog of war (Navigation/FogOfWar): needs the nav grid (baked on first use)
        sol::table fog = lua.create_named_table("fog");
        fog.set_function("enable", [this](sol::optional<sol::table> opts) {
            if (!Fog || !EnsureNav()) return false;
            FogOfWarState& s = ActiveScene->Fog;
            Fog->Enable(*Nav, s);
            if (opts) {
                s.HiddenColor = opts->get<sol::optional<glm::vec3>>("hidden_color").value_or(s.HiddenColor);
                s.ExploredColor = opts->get<sol::optional<glm::vec3>>("explored_color").value_or(s.ExploredColor);
                s.ExploredAlpha = opts->get<sol::optional<float>>("explored_alpha").value_or(s.ExploredAlpha);
                Fog->FadeSpeed = opts->get<sol::optional<float>>("fade").value_or(Fog->FadeSpeed);
            }
            return true;
        });
        fog.set_function("disable", [this]() { if (Fog) Fog->Disable(ActiveScene->Fog); });
        fog.set_function("enabled", [this]() { return Fog && Fog->IsEnabled(); });
        fog.set_function("reveal", [this](const glm::vec3& pos, float radius, sol::optional<float> eye) {
            if (Fog && Fog->IsEnabled() && EnsureNav()) Fog->Reveal(*Nav, *Physics, pos, radius, eye.value_or(1.6f));
        });
        fog.set_function("explore", [this](const glm::vec3& pos, float radius) { if (Fog) Fog->Explore(pos, radius); });
        fog.set_function("is_visible", [this](const glm::vec3& pos) { return !Fog || Fog->IsVisible(pos); });
        fog.set_function("is_explored", [this](const glm::vec3& pos) { return !Fog || Fog->IsExplored(pos); });
        fog.set_function("reset", [this]() { if (Fog) Fog->Reset(); });
        fog.set_function("save", [this]() { return Fog && Fog->IsEnabled() ? Fog->Save() : std::string(); });
        fog.set_function("load", [this](const std::string& data) { return Fog && Fog->IsEnabled() && Fog->Load(data); });

        // ---- line of sight and cover (static level geometry)
        auto sight = [](const sol::optional<sol::table>& opts) {
            NavGrid::SightOptions o;
            if (opts) {
                o.EyeHeight = opts->get<sol::optional<float>>("eye").value_or(o.EyeHeight);
                o.TargetHeight = opts->get<sol::optional<float>>("target").value_or(o.TargetHeight);
            }
            return o;
        };
        nav.set_function("line_of_sight", [this, sight](const glm::vec3& from, const glm::vec3& to, sol::optional<sol::table> opts)
            -> std::tuple<bool, sol::object> {
            if (!EnsureNav()) return { false, sol::make_object(*Lua, sol::lua_nil) };
            glm::vec3 hit(0.0f);
            const bool seen = Nav->LineOfSight(*Physics, from, to, sight(opts), &hit);
            return { seen, seen ? sol::make_object(*Lua, sol::lua_nil) : sol::make_object(*Lua, hit) };
        });
        nav.set_function("cover", [this](const glm::vec3& at, const glm::vec3& from) -> std::tuple<std::string, int> {
            if (!EnsureNav()) return { "none", 0 };
            const NavGrid::Cover c = Nav->CoverAt(*Physics, at, from);
            return { c == NavGrid::Cover::Full ? "full" : c == NavGrid::Cover::Half ? "half" : "none", (int)c };
        });
        nav.set_function("visible_cells", [this, sight](const glm::vec3& from, float range, sol::optional<sol::table> opts) {
            sol::table out = Lua->create_table();
            if (!EnsureNav()) return out;
            std::vector<int> cells;
            Nav->VisibleCells(*Physics, from, range, sight(opts), cells);
            for (size_t i = 0; i < cells.size(); i++)
                out[i + 1] = Lua->create_table_with("cell", cells[i], "position", Nav->CellPosition(cells[i]));
            return out;
        });
    }

    // ---------------------------------------------------------------- save games
    // Lua values <-> JSON. A table whose keys are exactly 1..n is an array, anything else an object.
    // Object keys: strings as they are ("#" in front escaped as "#s#..."), integers as "#<n>".
    // vec3 -> { "$vec3": [x, y, z] }, entity -> { "$entity": uid } (nil if it no longer exists).
    nlohmann::json ScriptEngine::Impl::ToJson(const sol::object& o, int depth) {
        using json = nlohmann::json;
        if (depth > 32) throw std::runtime_error("tables nested more than 32 deep (a table that contains itself?)");
        switch (o.get_type()) {
        case sol::type::lua_nil:
        case sol::type::none:
            return nullptr;
        case sol::type::boolean:
            return o.as<bool>();
        case sol::type::number: {
            lua_State* L = Lua->lua_state();
            o.push(L);
            const bool isInt = lua_isinteger(L, -1);
            const lua_Integer i = isInt ? lua_tointeger(L, -1) : 0;
            const double d = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (isInt) return (int64_t)i;
            if (!std::isfinite(d)) throw std::runtime_error("can't save inf or nan");
            return d;
        }
        case sol::type::string:
            return o.as<std::string>();
        case sol::type::userdata:
            if (o.is<glm::vec3>()) {
                const glm::vec3 v = o.as<glm::vec3>();
                return json{ { "$vec3", json::array({ v.x, v.y, v.z }) } };
            }
            if (o.is<ScriptEntity>()) {
                const ScriptEntity e = o.as<ScriptEntity>();
                return e.Valid() ? json{ { "$entity", e.Owner->GetID(e.Handle) } } : json(nullptr);
            }
            throw std::runtime_error("can't save this value (tables, numbers, strings, booleans, vec3 and entities can be saved)");
        case sol::type::table: {
            sol::table t = o.as<sol::table>();
            const size_t n = t.size();
            size_t count = 0;
            for (const auto& kv : t) { (void)kv; count++; }
            if (n > 0 && count == n) {
                json arr = json::array();
                for (size_t i = 1; i <= n; i++) { sol::object v = t[i]; arr.push_back(ToJson(v, depth + 1)); }
                return arr;
            }
            json obj = json::object();
            for (const auto& [k, v] : t) {
                std::string key;
                if (k.get_type() == sol::type::string) {
                    key = k.as<std::string>();
                    if (!key.empty() && key[0] == '#') key = "#s" + key;
                }
                else if (k.get_type() == sol::type::number) {
                    lua_State* L = Lua->lua_state();
                    k.push(L);
                    const bool isInt = lua_isinteger(L, -1);
                    const lua_Integer i = isInt ? lua_tointeger(L, -1) : 0;
                    lua_pop(L, 1);
                    if (!isInt) throw std::runtime_error("can't save a table with fractional number keys");
                    key = "#" + std::to_string(i);
                }
                else throw std::runtime_error("can't save a table with keys that are not strings or integers");
                obj[key] = ToJson(v, depth + 1);
            }
            return obj;
        }
        default:
            throw std::runtime_error("can't save a function or coroutine");
        }
    }

    sol::object ScriptEngine::Impl::FromJson(const nlohmann::json& j) {
        sol::state& lua = *Lua;
        switch (j.type()) {
        case nlohmann::json::value_t::boolean:         return sol::make_object(lua, j.get<bool>());
        case nlohmann::json::value_t::number_integer:  return sol::make_object(lua, (lua_Integer)j.get<int64_t>());
        case nlohmann::json::value_t::number_unsigned: return sol::make_object(lua, (lua_Integer)j.get<uint64_t>());
        case nlohmann::json::value_t::number_float:    return sol::make_object(lua, j.get<double>());
        case nlohmann::json::value_t::string:          return sol::make_object(lua, j.get<std::string>());
        case nlohmann::json::value_t::array: {
            sol::table t = lua.create_table((int)j.size(), 0);
            for (size_t i = 0; i < j.size(); i++) t[i + 1] = FromJson(j[i]);
            return t;
        }
        case nlohmann::json::value_t::object: {
            if (j.size() == 1 && j.contains("$vec3")) return sol::make_object(lua, j["$vec3"].get<glm::vec3>());
            if (j.size() == 1 && j.contains("$entity")) {
                const entt::entity h = ActiveScene->FindByID(j["$entity"].get<uint64_t>());
                return h != entt::null ? MakeEntity(h) : sol::make_object(lua, sol::lua_nil);
            }
            sol::table t = lua.create_table();
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string& key = it.key();
                if (key.rfind("#s", 0) == 0) t[key.substr(2)] = FromJson(it.value());
                else if (key.size() > 1 && key[0] == '#') t[(lua_Integer)std::stoll(key.substr(1))] = FromJson(it.value());
                else t[key] = FromJson(it.value());
            }
            return t;
        }
        default:
            return sol::make_object(lua, sol::lua_nil);
        }
    }

    void ScriptEngine::Impl::BindSaves() {
        sol::state& lua = *Lua;
        sol::table scene = lua["scene"];
        scene.set_function("is_loading", [this]() { return Loading; });
        scene.set_function("find_uid", [this](int64_t uid) -> sol::object {
            const entt::entity h = ActiveScene->FindByID((uint64_t)uid);
            return h != entt::null ? MakeEntity(h) : sol::make_object(*Lua, sol::lua_nil);
        });

        sol::table saves = lua.create_named_table("saves");
        saves.set_function("save", [this](const std::string& slot, sol::optional<std::string> name) {
            std::string error = "saving is not available here";
            const bool ok = Host && Host->SaveGame(slot, name.value_or(slot), error);
            return std::make_tuple(ok, ok ? sol::make_object(*Lua, sol::lua_nil) : sol::make_object(*Lua, error));
        });
        saves.set_function("load", [this](const std::string& slot) { return Host && Host->RequestLoad(slot); });
        saves.set_function("delete", [this](const std::string& slot) { return Host && Host->DeleteSave(slot); });
        saves.set_function("list", [this]() {
            return Host ? FromJson(Host->ListSaves()) : sol::make_object(*Lua, Lua->create_table());
        });
        saves.set_function("exists", [this](const std::string& slot) {
            if (!Host) return false;
            for (const auto& s : Host->ListSaves()) if (s.value("slot", std::string()) == slot) return true;
            return false;
        });

        sol::table level = lua.create_named_table("level");
        level.set_function("change", [this](const std::string& file, sol::optional<std::string> entry, sol::optional<sol::table> opts) {
            if (!Host) throw std::runtime_error("level.change is not available here");
            std::vector<uint64_t> carry;
            if (opts) {
                if (auto list = opts->get<sol::optional<sol::table>>("carry"))
                    for (const auto& [k, v] : *list)
                        if (v.is<ScriptEntity>() && v.as<ScriptEntity>().Valid())
                            carry.push_back(ActiveScene->GetID(v.as<ScriptEntity>().Handle));
            }
            Host->RequestLevel(file, entry.value_or(std::string()), carry);
        });
        level.set_function("current", [this]() { return Host ? Host->CurrentLevel() : std::string(); });
        level.set_function("entry", [this]() -> sol::object {
            const std::string e = Host ? Host->LevelEntry() : std::string();
            return e.empty() ? sol::make_object(*Lua, sol::lua_nil) : sol::make_object(*Lua, e);
        });
        level.set_function("play_time", [this]() { return Host ? Host->PlayTime() : 0.0f; });
        level.set_function("arrived", [this]() { return Host && Host->Arrived(); });

        // ---- events: one script (or a dialogue) tells every script something happened
        sol::table events = lua.create_named_table("events");
        events.set_function("emit", [this](const std::string& name, sol::optional<sol::object> data) {
            if (Events.size() >= 1024) throw std::runtime_error("events.emit: too many events this frame (an endless loop?)");
            Events.push_back({ name, data ? *data : sol::make_object(*Lua, sol::lua_nil) });
        });

        // ---- data files: any JSON under Assets as a Lua table (dialogues, item tables, quest lists)
        sol::table dataT = lua.create_named_table("data");
        dataT.set_function("load", [this](const std::string& path) -> std::tuple<sol::object, sol::object> {
            std::ifstream in(AssetDir + path);
            if (!in) return { sol::make_object(*Lua, sol::lua_nil), sol::make_object(*Lua, "can't open " + path) };
            try {
                nlohmann::json j;
                in >> j;
                return { FromJson(j), sol::make_object(*Lua, sol::lua_nil) };
            }
            catch (const std::exception& ex) {
                return { sol::make_object(*Lua, sol::lua_nil), sol::make_object(*Lua, path + ": " + ex.what()) };
            }
        });
    }

    // Every queued event goes to every script with on_event(self, name, data). Events emitted meanwhile
    // are delivered in the same call, a few rounds deep at most.
    void ScriptEngine::Impl::DispatchEvents() {
        for (int round = 0; round < 8 && !Events.empty(); round++) {
            std::vector<QueuedEvent> batch;
            batch.swap(Events);
            std::vector<entt::entity> handles;
            for (auto& [h, inst] : Instances) handles.push_back(h);
            for (const QueuedEvent& ev : batch)
                for (entt::entity h : handles) {
                    auto it = Instances.find(h);
                    if (it == Instances.end() || !it->second.Enabled || !it->second.OnEvent.valid()) continue;
                    Check(it->second.OnEvent(it->second.Self, ev.Name, ev.Data), it->second, "on_event");
                }
        }
        if (!Events.empty()) {
            ReportError("[script] events: still emitting after 8 rounds (events that emit each other?); dropped "
                + std::to_string(Events.size()));
            Events.clear();
        }
    }

    // Scripts/Lib/*.lua (in name order) run once in the shared globals when scripts start: code every
    // script can call, without each one loading it. The engine's libraries first (EngineAssets/Scripts/Lib:
    // the dialogue runner), then the project's own (Assets/Scripts/Lib), which may build on them.
    void ScriptEngine::Impl::LoadLibraries() {
        namespace fs = std::filesystem;
        for (const std::string& root : { EngineDir, AssetDir }) {
            std::error_code ec;
            if (root.empty()) continue;
            const fs::path dir = fs::u8path(root) / "Scripts" / "Lib";
            if (!fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> files;
            for (const auto& f : fs::directory_iterator(dir, ec))
                if (f.path().extension() == ".lua") files.push_back(f.path());
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) {
                auto r = Lua->safe_script_file(f.string(), sol::script_pass_on_error);
                if (!r.valid()) {
                    sol::error err = r;
                    ReportError("[script] Scripts/Lib/" + f.filename().string() + " (load): " + err.what());
                }
            }
        }
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
        inst.OnAnimEvent = inst.Env["on_anim_event"];
        inst.OnSave = inst.Env["on_save"];
        inst.OnLoad = inst.Env["on_load"];
        inst.OnEvent = inst.Env["on_event"];

        sol::protected_function onCreate = inst.Env["on_create"];
        auto& stored = Instances.emplace(handle, std::move(inst)).first->second;
        if (onCreate.valid()) Check(onCreate(stored.Self), stored, "on_create");

        // A restored level: hand the script what its on_save returned
        if (RestoreScripts.empty()) return;
        auto data = RestoreScripts.find(std::to_string(ActiveScene->GetID(handle)));
        auto it = Instances.find(handle);
        if (data == RestoreScripts.end() || it == Instances.end() || !it->second.Enabled || !it->second.OnLoad.valid()) return;
        sol::object value;
        try { value = FromJson(*data); }
        catch (const std::exception& ex) { ReportError("[script] " + path + " (on_load data): " + ex.what()); return; }
        Check(it->second.OnLoad(it->second.Self, value), it->second, "on_load");
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
    ScriptEngine::ScriptEngine(std::string assetDir, std::string engineDir, PhysicsWorld* physics, AudioEngine* audio) : m_Impl(std::make_unique<Impl>()) {
        m_Impl->EngineDir = std::move(engineDir);
        m_Impl->AssetDir = std::move(assetDir);
        m_Impl->Physics = physics;
        m_Impl->Audio = audio;
    }

    ScriptEngine::~ScriptEngine() = default;

    void ScriptEngine::SetResources(const ResourceRegistry& resources) { m_Impl->Resources = resources; }
    void ScriptEngine::SetPrefabs(PrefabLibrary* prefabs) { m_Impl->Prefabs = prefabs; }
    void ScriptEngine::SetNavigation(NavGrid* nav) { m_Impl->Nav = nav; }
    void ScriptEngine::SetFog(FogOfWar* fog) { m_Impl->Fog = fog; }
    void ScriptEngine::SetViewAspect(float aspect) { if (aspect > 0.0f) m_Impl->ViewAspect = aspect; }
    void ScriptEngine::SetHost(ScriptHost* host) { m_Impl->Host = host; }

    void ScriptEngine::SetRestore(nlohmann::json state, bool loading) {
        m_Impl->PendingRestore = std::move(state);
        m_Impl->HasPendingRestore = true;
        m_Impl->PendingLoading = loading;
    }

    nlohmann::json ScriptEngine::CaptureState(const std::function<bool(uint64_t)>& include) {
        Impl& m = *m_Impl;
        nlohmann::json out = { { "game", nlohmann::json::object() }, { "shared", nlohmann::json::object() },
                               { "scripts", nlohmann::json::object() } };
        if (!m.Lua || !m.ActiveScene) return out;
        for (const char* name : { "game", "shared" }) {
            try { sol::object v = (*m.Lua)[name]; out[name] = m.ToJson(v); }
            catch (const std::exception& ex) { m.ReportError(std::string("[script] saving ") + name + ": " + ex.what()); }
        }
        auto& reg = m.ActiveScene->Registry();
        std::vector<entt::entity> handles;               // on_save may (wrongly) start or stop scripts
        for (auto& [h, inst] : m.Instances) handles.push_back(h);
        for (entt::entity h : handles) {
            auto it = m.Instances.find(h);
            if (it == m.Instances.end() || !reg.valid(h) || reg.all_of<TransientComponent>(h)) continue;
            ScriptInstance& inst = it->second;
            if (!inst.Enabled || !inst.OnSave.valid()) continue;
            const uint64_t uid = m.ActiveScene->GetID(h);
            if (include && !include(uid)) continue;
            auto result = inst.OnSave(inst.Self);
            if (!m.Check(result, inst, "on_save")) continue;
            sol::object value = result;
            if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) continue;
            try { out["scripts"][std::to_string(uid)] = m.ToJson(value); }
            catch (const std::exception& ex) { m.ReportError("[script] " + inst.Path + " (on_save): " + ex.what()); }
        }
        return out;
    }

    bool ScriptEngine::IsRunning() const { return m_Impl->Lua != nullptr; }
    uint32_t ScriptEngine::InstanceCount() const { return (uint32_t)m_Impl->Instances.size(); }
    const std::vector<std::string>& ScriptEngine::RecentErrors() const { return m_Impl->Errors; }
    const std::vector<ScriptUIItem>& ScriptEngine::UIItems() const { return m_Impl->UI; }

    void ScriptEngine::SetPointer(float x, float y, bool inside, bool down, float wheel, bool rightDown) {
        Impl& m = *m_Impl;
        m.PtrClicked = down && !m.PtrDown && inside;      // pressed this frame, over the view
        m.PtrX = x;
        m.PtrY = y;
        m.PtrInside = inside;
        m.PtrDown = down;
        m.PtrWheel = wheel;
        m.PtrRightDown = rightDown;
    }

    void ScriptEngine::SetUIFonts(ImFont* bold, ImFont* regular) { m_Impl->UIC.SetFonts(bold, regular); }

    bool ScriptEngine::ConsumeQuitRequest() {
        const bool q = m_Impl->QuitRequested;
        m_Impl->QuitRequested = false;
        return q;
    }

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
        m.NavTried = false;
        m.NavConfig = NavSettings{};
        m.UIC.Reset();
        m.DragPayload = sol::object();
        m.SimPointer = false;
        m.UIWarned = false;
        m.Errors.clear();
        m.Lua = std::make_unique<sol::state>();
        m.BindApi();
        m.SampleInput();
        m.KeyPrev = m.KeyNow;       // keys already held when Play starts don't count as pressed
        m.MousePrev = m.MouseNow;

        // A restored level or a level change: `game` and `shared` come back before any on_create
        if (m.HasPendingRestore) {
            const nlohmann::json& r = m.PendingRestore;
            for (const char* name : { "game", "shared" }) {
                auto it = r.find(name);
                if (it == r.end() || !(it->is_object() || it->is_array())) continue;
                try { (*m.Lua)[name] = m.FromJson(*it); }
                catch (const std::exception& ex) { m.ReportError(std::string("[script] restoring ") + name + ": " + ex.what()); }
            }
            if (auto it = r.find("scripts"); it != r.end() && it->is_object()) m.RestoreScripts = *it;
            m.Loading = m.PendingLoading;
            m.HasPendingRestore = false;
            m.PendingRestore = nullptr;
        }

        m.LoadLibraries();          // Scripts/Lib/*.lua: shared code (e.g. the dialogue runner) in the globals

        // Start everything now so on_create runs before the first frame
        m.StartNewInstances(scene.Registry());
        m.FlushDestroys(scene);
        m.Loading = false;
        m.RestoreScripts = nlohmann::json::object();
        ENGINE_INFO("Scripts started: {} running", m.Instances.size());
    }

    void ScriptEngine::Update(Scene& scene, float dt) {
        Impl& m = *m_Impl;
        if (!m.Lua) return;
        m.ActiveScene = &scene;
        m.Time += dt;
        m.UI.clear();               // a paused game keeps showing the last frame's UI (and decals)
        scene.Decals.clear();       // decal.draw is per frame too
        if (m.Fog) m.Fog->BeginFrame();   // fog.reveal is per frame too
        {
            UIPointerState p{ m.PtrX, m.PtrY, m.PtrInside, m.PtrDown, m.PtrRightDown, m.PtrWheel };
            if (m.SimPointer) p = m.Sim;                                  // ui.simulate_pointer (tests)
            m.UIC.BeginFrame(m.UI, m.ViewAspect, p, dt);
            if (m.UIC.DragEndedLastFrame()) m.DragPayload = sol::object();
        }
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

        m.DispatchEvents();         // events.emit from on_update (on_event may destroy things too)
        if (m.Fog) m.Fog->EndFrame(scene.Fog, dt);   // what was revealed this frame: explored, faded, drawn
        m.FlushDestroys(scene);
        if (const int open = m.UIC.EndFrame(); open > 0 && !m.UIWarned) {   // a begin_* without its end_*
            m.UIWarned = true;
            m.ReportError("[script] ui: " + std::to_string(open) + " ui.begin_* call(s) without ui.end_* (closed automatically)");
        }
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

    void ScriptEngine::DispatchAnimationEvents(Scene& scene) {
        Impl& m = *m_Impl;
        if (!m.Lua) return;
        m.ActiveScene = &scene;

        // Copy first: a callback may change animators (play another clip, add events)
        std::vector<std::tuple<entt::entity, std::string, std::string>> fired;
        for (auto [e, an] : scene.Registry().view<AnimatorComponent>().each())
            for (const auto& [name, clip] : an.Fired) fired.emplace_back(e, name, clip);

        for (const auto& [e, name, clip] : fired) {
            if (!scene.Registry().valid(e)) continue;
            if (name.rfind("sound:", 0) == 0) {           // a sound at the entity, no script needed
                if (m.Audio) m.Audio->PlayAt(name.substr(6), glm::vec3(scene.GetWorldMatrix(e)[3]), 1.0f, 1.0f);
                continue;
            }
            auto it = m.Instances.find(e);
            if (it == m.Instances.end() || !it->second.Enabled || !it->second.OnAnimEvent.valid()) continue;
            ScriptInstance& inst = it->second;
            m.Check(inst.OnAnimEvent(inst.Self, name, clip), inst, "on_anim_event");
        }
        m.FlushDestroys(scene);
    }

    void ScriptEngine::Stop(Scene& scene) {
        Impl& m = *m_Impl;
        if (!m.Lua) return;
        m.ActiveScene = &scene;
        m.DragPayload = sol::object();   // before the Lua state goes
        scene.Decals.clear();

        std::vector<entt::entity> handles;
        for (auto& [h, inst] : m.Instances) handles.push_back(h);
        for (auto h : handles)
            m.DestroyInstance(h, scene.Registry().valid(h));

        m.Instances.clear();
        m.Failed.clear();
        m.PendingDestroy.clear();   // the scene is about to be restored anyway
        m.PendingRestart.clear();
        m.UI.clear();
        m.QuitRequested = false;
        m.RestartRequested = false;
        m.Events.clear();           // they hold Lua values
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
