// Particle emitters: CPU simulation (Scene::UpdateParticles), the per-frame draw lists
// (Scene::GatherParticles, drawn by ParticleRenderer) and presets.
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/ParticleRenderer.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <glm/gtc/constants.hpp>

namespace Engine {

    namespace {
        std::mt19937& Rng() {
            static thread_local std::mt19937 rng{ 0x5eed1234u };
            return rng;
        }
        float Rand(float a, float b) {
            if (b < a) std::swap(a, b);
            return std::uniform_real_distribution<float>(a, b)(Rng());
        }

        // A random direction within `spreadDeg` of `dir` (uniform over the cone's cap)
        glm::vec3 InCone(const glm::vec3& dir, float spreadDeg) {
            const glm::vec3 d = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0, 1, 0);
            const float cosMax = std::cos(glm::radians(std::clamp(spreadDeg, 0.0f, 180.0f)));
            const float cosT = Rand(cosMax, 1.0f), sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
            const float phi = Rand(0.0f, glm::two_pi<float>());
            const glm::vec3 helper = std::abs(d.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 t = glm::normalize(glm::cross(helper, d)), b = glm::cross(d, t);
            return glm::normalize(d * cosT + (t * std::cos(phi) + b * std::sin(phi)) * sinT);
        }

        glm::vec3 InShape(const ParticleEmitterComponent& pe) {
            switch (pe.Shape) {
            case EmitShape::Sphere: {
                const glm::vec3 p(Rand(-1, 1), Rand(-1, 1), Rand(-1, 1));          // a direction, then a radius
                return (glm::length(p) > 1e-6f ? glm::normalize(p) : glm::vec3(0)) * pe.ShapeSize.x * std::cbrt(Rand(0, 1));
            }
            case EmitShape::Box:
                return glm::vec3(Rand(-0.5f, 0.5f), Rand(-0.5f, 0.5f), Rand(-0.5f, 0.5f)) * pe.ShapeSize;
            default: return glm::vec3(0.0f);
            }
        }

        void Emit(ParticleEmitterComponent& pe, int count, const glm::mat4& world) {
            count = std::min(count, std::max(0, pe.MaxParticles - (int)pe.Particles.size()));
            const glm::mat3 basis(world);
            for (int i = 0; i < count; i++) {
                Particle p;
                const glm::vec3 local = InShape(pe);
                const glm::vec3 dir = InCone(pe.Direction, pe.Spread);
                const float speed = Rand(pe.Speed.x, pe.Speed.y);
                if (pe.WorldSpace) {
                    p.Position = glm::vec3(world * glm::vec4(local, 1.0f));
                    const glm::vec3 wd = basis * dir;
                    p.Velocity = (glm::length(wd) > 1e-6f ? glm::normalize(wd) : glm::vec3(0, 1, 0)) * speed;
                }
                else {
                    p.Position = local;
                    p.Velocity = dir * speed;
                }
                p.Life = std::max(0.01f, Rand(pe.Lifetime.x, pe.Lifetime.y));
                p.Size = std::max(0.0f, Rand(pe.Size.x, pe.Size.y));
                const bool spins = pe.SpinSpeed.x != 0.0f || pe.SpinSpeed.y != 0.0f;
                p.Rotation = spins ? Rand(0.0f, glm::two_pi<float>()) : 0.0f;
                p.Spin = glm::radians(Rand(pe.SpinSpeed.x, pe.SpinSpeed.y));
                pe.Particles.push_back(p);
            }
        }

        glm::vec3 Linear(const glm::vec4& c) { return glm::pow(glm::max(glm::vec3(c), glm::vec3(0.0f)), glm::vec3(2.2f)); }
    }

    // ------------------------------------------------------------------ simulation
    void Scene::UpdateParticles(float dt) {
        if (dt <= 0.0f) return;
        dt = std::min(dt, 0.1f);                                  // a long hitch doesn't fire a huge wave
        std::vector<entt::entity> finished;
        for (auto [e, pe] : m_Registry.view<ParticleEmitterComponent>().each()) {
            const glm::mat4 world = GetWorldMatrix(e);
            // Gravity in the particles' space (world, or the emitter's own when they move with it)
            glm::vec3 gravity(0.0f, -9.81f * pe.GravityScale, 0.0f);
            if (!pe.WorldSpace) gravity = glm::inverse(glm::mat3(world)) * gravity;
            const float keep = std::max(0.0f, 1.0f - pe.Drag * dt);

            for (size_t i = 0; i < pe.Particles.size();) {
                Particle& p = pe.Particles[i];
                p.Age += dt;
                if (p.Age >= p.Life) { p = pe.Particles.back(); pe.Particles.pop_back(); continue; }
                p.Velocity = (p.Velocity + gravity * dt) * keep;
                p.Position += p.Velocity * dt;
                p.Rotation += p.Spin * dt;
                i++;
            }

            if (!pe.Started) { pe.Started = true; Emit(pe, pe.Burst, world); }
            if (pe.PendingBurst > 0) { Emit(pe, pe.PendingBurst, world); pe.PendingBurst = 0; }
            if (pe.Emitting && pe.Rate > 0.0f) {
                pe.SpawnCarry += pe.Rate * dt;
                const int n = (int)pe.SpawnCarry;
                pe.SpawnCarry -= (float)n;
                Emit(pe, n, world);
            }
            if (pe.DestroyWhenDone && !pe.Emitting && pe.Particles.empty()) finished.push_back(e);
        }
        for (auto e : finished) if (m_Registry.valid(e)) DestroyEntity(Entity(e, this));
    }

    // ------------------------------------------------------------------ drawing
    void Scene::GatherParticles(const Camera& camera, std::vector<ParticleDraw>& draws) {
        draws.clear();
        for (auto [e, pe] : m_Registry.view<ParticleEmitterComponent>().each()) {
            if (pe.Particles.empty()) continue;
            const glm::mat4 world = GetWorldMatrix(e);
            ParticleDraw d;
            d.Texture = pe.Texture.get();
            d.Additive = pe.Additive;
            d.Distance = glm::length(glm::vec3(world[3]) - camera.Position);
            d.Instances.reserve(pe.Particles.size());

            const glm::vec3 c0 = Linear(pe.StartColor) * pe.Emission, c1 = Linear(pe.EndColor) * pe.Emission;
            std::vector<float> depth;
            if (!pe.Additive) depth.reserve(pe.Particles.size());
            for (const Particle& p : pe.Particles) {
                const float t = std::clamp(p.Age / p.Life, 0.0f, 1.0f);
                const glm::vec3 pos = pe.WorldSpace ? p.Position : glm::vec3(world * glm::vec4(p.Position, 1.0f));
                ParticleInstance inst;
                inst.PositionSize = glm::vec4(pos, p.Size * glm::mix(1.0f, pe.EndSize, t));
                inst.Color = glm::vec4(glm::mix(c0, c1, t), glm::mix(pe.StartColor.a, pe.EndColor.a, t));
                inst.Extra = glm::vec4(p.Rotation, 0.0f, 0.0f, 0.0f);
                d.Instances.push_back(inst);
            }
            // "Over" blending needs far particles first; adding light doesn't care about order
            if (!pe.Additive) {
                std::sort(d.Instances.begin(), d.Instances.end(), [&](const ParticleInstance& a, const ParticleInstance& b) {
                    const glm::vec3 da = glm::vec3(a.PositionSize) - camera.Position, db = glm::vec3(b.PositionSize) - camera.Position;
                    return glm::dot(da, da) > glm::dot(db, db);
                });
            }
            draws.push_back(std::move(d));
        }
    }

    uint32_t Scene::ParticleCount() const {
        uint32_t n = 0;
        for (auto [e, pe] : m_Registry.view<const ParticleEmitterComponent>().each()) n += (uint32_t)pe.Particles.size();
        return n;
    }

    void Scene::ResetParticles() {
        for (auto [e, pe] : m_Registry.view<ParticleEmitterComponent>().each()) {
            pe.Particles.clear();
            pe.Started = false;
            pe.SpawnCarry = 0.0f;
            pe.PendingBurst = 0;
        }
    }

    // ------------------------------------------------------------------ presets
    const std::vector<std::string>& ParticlePresetNames() {
        static const std::vector<std::string> names = { "Fire", "Smoke", "Sparks", "Magic", "Dust", "Embers" };
        return names;
    }

    bool ApplyParticlePreset(ParticleEmitterComponent& pe, const std::string& name) {
        ParticleEmitterComponent p;              // the defaults, then the preset's changes
        p.Texture = pe.Texture;
        if (name == "Fire") {
            p.Rate = 70.0f; p.Lifetime = { 0.6f, 1.1f }; p.Speed = { 1.0f, 1.8f }; p.Spread = 10.0f;
            p.Shape = EmitShape::Sphere; p.ShapeSize = glm::vec3(0.18f);
            p.Size = { 0.3f, 0.45f }; p.EndSize = 0.15f; p.Drag = 0.9f;
            p.StartColor = { 1.0f, 0.75f, 0.3f, 0.9f }; p.EndColor = { 0.9f, 0.15f, 0.02f, 0.0f };
            p.Emission = 2.2f; p.Additive = true; p.SpinSpeed = { -90.0f, 90.0f };
        }
        else if (name == "Smoke") {
            p.Rate = 12.0f; p.Lifetime = { 3.0f, 5.0f }; p.Speed = { 0.3f, 0.6f }; p.Spread = 15.0f;
            p.Shape = EmitShape::Sphere; p.ShapeSize = glm::vec3(0.2f);
            p.Size = { 0.5f, 0.8f }; p.EndSize = 3.5f; p.Drag = 0.2f;
            p.StartColor = { 0.32f, 0.32f, 0.35f, 0.75f }; p.EndColor = { 0.55f, 0.55f, 0.58f, 0.0f };
            p.Emission = 1.0f; p.Additive = false; p.SpinSpeed = { -20.0f, 20.0f };
        }
        else if (name == "Sparks") {
            p.Emitting = false; p.Burst = 40; p.Rate = 0.0f; p.Lifetime = { 0.5f, 1.1f }; p.Speed = { 3.0f, 6.0f };
            p.Spread = 70.0f; p.GravityScale = 1.0f; p.Drag = 0.6f;
            p.Size = { 0.04f, 0.08f }; p.EndSize = 0.3f;
            p.StartColor = { 1.0f, 0.9f, 0.5f, 1.0f }; p.EndColor = { 1.0f, 0.4f, 0.1f, 0.0f };
            p.Emission = 6.0f; p.Additive = true;
        }
        else if (name == "Magic") {
            p.Rate = 40.0f; p.Lifetime = { 1.0f, 2.0f }; p.Speed = { 0.2f, 0.6f }; p.Spread = 180.0f;
            p.Shape = EmitShape::Sphere; p.ShapeSize = glm::vec3(0.6f);
            p.Size = { 0.06f, 0.14f }; p.EndSize = 0.0f; p.GravityScale = -0.05f;
            p.StartColor = { 0.5f, 0.8f, 1.0f, 1.0f }; p.EndColor = { 0.8f, 0.4f, 1.0f, 0.0f };
            p.Emission = 4.0f; p.Additive = true;
        }
        else if (name == "Dust") {
            p.Emitting = false; p.Burst = 24; p.Rate = 0.0f; p.Lifetime = { 0.6f, 1.2f }; p.Speed = { 0.6f, 1.4f };
            p.Direction = { 0.0f, 1.0f, 0.0f }; p.Spread = 80.0f; p.GravityScale = 0.15f; p.Drag = 2.0f;
            p.Size = { 0.15f, 0.3f }; p.EndSize = 2.5f;
            p.StartColor = { 0.75f, 0.68f, 0.55f, 0.45f }; p.EndColor = { 0.8f, 0.75f, 0.65f, 0.0f };
            p.Emission = 1.0f; p.Additive = false;
        }
        else if (name == "Embers") {
            p.Rate = 8.0f; p.Lifetime = { 1.5f, 3.0f }; p.Speed = { 0.4f, 1.0f }; p.Spread = 20.0f;
            p.Shape = EmitShape::Box; p.ShapeSize = glm::vec3(1.0f, 0.0f, 1.0f);
            p.Size = { 0.04f, 0.08f }; p.EndSize = 0.2f; p.GravityScale = -0.05f; p.Drag = 0.3f;
            p.StartColor = { 1.0f, 0.55f, 0.15f, 1.0f }; p.EndColor = { 1.0f, 0.15f, 0.02f, 0.0f };
            p.Emission = 5.0f; p.Additive = true;
        }
        else return false;
        // Keep the live state (so switching presets in the Inspector doesn't drop what is on screen)
        p.Particles = std::move(pe.Particles);
        p.Started = pe.Started;
        p.DestroyWhenDone = pe.DestroyWhenDone;
        pe = std::move(p);
        return true;
    }

} // namespace Engine
