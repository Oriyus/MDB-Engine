#pragma once
#include "Renderer/Texture.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine {

    struct Material {
        std::string Name;
        glm::vec3 Color{ 1.0f };                // sRGB-style, like the Inspector's color picker
        std::shared_ptr<Texture2D> Albedo;      // optional
        float Shininess = 64.0f;
        float SpecularStrength = 0.5f;
    };

} // namespace Engine