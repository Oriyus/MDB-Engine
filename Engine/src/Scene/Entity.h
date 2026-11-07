#pragma once
#include "Scene/Scene.h"
#include <entt/entt.hpp>
#include <utility>

namespace Engine {

    class Entity {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene) : m_Handle(handle), m_Scene(scene) {}

        template<typename T, typename... Args>
        T& Add(Args&&... args) {
            return m_Scene->Registry().emplace_or_replace<T>(m_Handle, std::forward<Args>(args)...);
        }

        template<typename T>
        T& Get() { return m_Scene->Registry().get<T>(m_Handle); }

        template<typename T>
        bool Has() const { return m_Scene->Registry().all_of<T>(m_Handle); }

        template<typename T>
        void Remove() { m_Scene->Registry().remove<T>(m_Handle); }

        bool IsValid() const { return m_Scene && m_Scene->Registry().valid(m_Handle); }
        explicit operator bool() const { return IsValid(); }

        entt::entity GetHandle() const { return m_Handle; }
        Scene* GetScene() const { return m_Scene; }

    private:
        entt::entity m_Handle{ entt::null };
        Scene* m_Scene = nullptr;
    };

} // namespace Engine