#pragma once
#include <entt/entt.hpp>
#include <string>

namespace Engine {

    class Entity;
    class Camera;
    class Shader;
    class ShadowContext;

    class Scene {
    public:
        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyEntity(Entity entity);

        void Clear();
        void OnUpdate(float dt);
        void RenderShadows(ShadowContext& ctx);
        void Render(const Camera& camera, Shader& shader, const ShadowContext* shadows = nullptr);

        entt::registry& Registry() { return m_Registry; }

    private:
        entt::registry m_Registry;
    };

} // namespace Engine