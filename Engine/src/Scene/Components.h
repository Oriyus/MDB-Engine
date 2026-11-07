#pragma once
#include "Renderer/Renderable.h"
#include "Renderer/Texture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

namespace Engine {

    // Every entity gets these two automatically (see Scene::CreateEntity)
    struct TagComponent {
        std::string Tag;
    };

    struct TransformComponent {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f };   // Euler angles in radians
        glm::vec3 Scale{ 1.0f };

        glm::mat4 GetMatrix() const {
            return glm::translate(glm::mat4(1.0f), Position)
                * glm::mat4_cast(glm::quat(Rotation))
                * glm::scale(glm::mat4(1.0f), Scale);
        }
    };

    // Geometry + material in one component for now. We'll split material out later.
    struct MeshRendererComponent {
        std::shared_ptr<Renderable> Geometry;
        std::shared_ptr<Texture2D>  Texture;      // optional
        glm::vec3 Color{ 1.0f };                  // tints the texture, or is the flat color
        glm::mat4 LocalOffset{ 1.0f };            // applied before the entity transform (used by models)
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        glm::vec3 Emissive{ 0.0f };               // linear, can exceed 1
        bool  Unlit = false;
        bool  UseModelMaterials = false;   // use the model file's materials; Color becomes a tint
    };

    struct PointLightComponent {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        bool  CastShadows = true;
    };

    // Direction comes from the entity's rotation: the light travels along local -Z.
    // Position doesn't matter for the light itself (the entity just holds a marker).
    struct DirectionalLightComponent {
        glm::vec3 Color{ 1.0f, 0.95f, 0.9f };
        float Intensity = 0.4f;
        bool  CastShadows = true;
    };

    // Behaviour components: each one is paired with a system in Scene::OnUpdate
    struct SpinComponent {
        glm::vec3 Speed{ 0.0f };      // radians per second around each axis
    };

    struct OrbitComponent {
        glm::vec3 Center{ 0.0f };
        float Radius = 3.0f;
        float Height = 1.5f;
        float Speed = 1.0f;          // radians per second
        float Angle = 0.0f;
    };

} // namespace Engine