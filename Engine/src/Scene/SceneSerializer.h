#pragma once
#include "Scene/Scene.h"
#include "Renderer/PostProcess.h"
#include "Renderer/Shadowmap.h"

#include <glm/glm.hpp>
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

    // Everything about a level that isn't an entity. The scene's ambient intensity is saved
    // with it too (it lives on Scene). The app copies these in before Save and out after Load.
    struct SceneSettings {
        // Environment
        std::string EnvironmentMap;      // file name in Assets/Environments; empty = procedural sky
        bool  ShowSky = true;
        float SkyBlur = 0.0f;
        float SkyRotation = 0.0f;        // degrees

        PostSettings Post;
        ShadowSettings Shadows;

        // SSAO and bloom tuning (members of those passes, not of PostSettings)
        float AORadius = 0.5f, AOBias = 0.025f;
        int   AOSamples = 16;
        float BloomThreshold = 1.0f, BloomKnee = 0.5f, BloomRadius = 1.0f, BloomMaxBrightness = 8.0f;

        float Gravity = 9.81f;           // m/s^2, downward

        // Editor camera
        glm::vec3 CameraPosition{ 0.0f, 1.0f, 6.0f };
        float CameraYaw = -90.0f, CameraPitch = 0.0f;   // degrees
        float CameraFocusDistance = 6.0f;
    };

    class SceneSerializer {
    public:
        SceneSerializer(Scene& scene, ResourceRegistry resources)
            : m_Scene(scene), m_Resources(std::move(resources)) {}

        // settings may be null: then only the entities are written / read.
        // On Load, sections missing from the file (older files) leave those settings untouched,
        // so pass in the current values. Nothing changes unless the whole file parses.
        bool Save(const std::string& path, const SceneSettings* settings = nullptr) const;
        bool Load(const std::string& path, SceneSettings* settings = nullptr);   // replaces the scene's contents

    private:
        Scene& m_Scene;
        ResourceRegistry m_Resources;
    };

} // namespace Engine
