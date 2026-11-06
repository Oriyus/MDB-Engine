#pragma once
#include "Scene/Scene.h"

#include <functional>
#include <memory>
#include <string>

namespace Engine {

    class Renderable;
    class Texture2D;

    // Maps names stored in the file to live resources, and back
    struct ResourceRegistry {
        std::function<std::string(const std::shared_ptr<Renderable>&)> GeometryName;
        std::function<std::shared_ptr<Renderable>(const std::string&)> FindGeometry;
        std::function<std::string(const std::shared_ptr<Texture2D>&)>  TextureName;
        std::function<std::shared_ptr<Texture2D>(const std::string&)>  FindTexture;
    };

    class SceneSerializer {
    public:
        SceneSerializer(Scene& scene, ResourceRegistry resources)
            : m_Scene(scene), m_Resources(std::move(resources)) {}

        bool Save(const std::string& path) const;
        bool Load(const std::string& path);   // replaces the scene's contents

    private:
        Scene& m_Scene;
        ResourceRegistry m_Resources;
    };

} // namespace Engine