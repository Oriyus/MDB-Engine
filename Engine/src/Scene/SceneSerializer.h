#pragma once
#include "Scene/Scene.h"
#include "Renderer/PostProcess.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/VolumetricFog.h"

#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <string>

namespace Engine {

    class Renderable;
    class Texture2D;
    class TerrainData;
    struct AnimGraph;
    struct GameDatabase;
    struct UIDocument;
    class PrefabLibrary;

    // Maps names stored in the file to live resources, and back
    struct ResourceRegistry {
        std::function<std::string(const std::shared_ptr<Renderable>&)> GeometryName;
        std::function<std::shared_ptr<Renderable>(const std::string&)> FindGeometry;
        std::function<std::string(const std::shared_ptr<Texture2D>&)>  TextureName;
        std::function<std::shared_ptr<Texture2D>(const std::string&)>  FindTexture;
        // A terrain's heights and paint from its file under Assets ("Terrains/x.terrain"); null if there is none
        std::function<std::shared_ptr<TerrainData>(const std::string&)> LoadTerrain;
        // An animation graph ("AnimGraphs/x.json"); null if missing or unreadable
        std::function<std::shared_ptr<const AnimGraph>(const std::string&)> LoadAnimGraph;
        // The game database (Assets/Data/database.json); an empty one if there is no file. Cheap to call (cached).
        std::function<std::shared_ptr<const GameDatabase>()> LoadDatabase;
        // A UI layout ("UI/x.json"); null if missing or unreadable. Cheap to call (cached until the file changes).
        std::function<std::shared_ptr<const UIDocument>(const std::string&)> LoadUIDocument;
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

        VolumetricFogSettings VolumetricFog;
        float Gravity = 9.81f;           // m/s^2, downward

        // Editor camera
        glm::vec3 CameraPosition{ 0.0f, 1.0f, 6.0f };
        float CameraYaw = -90.0f, CameraPitch = 0.0f;   // degrees
        float CameraFocusDistance = 6.0f;
    };

    class SceneSerializer {
    public:
        // prefabs: resolves prefab links on Load and lets Save store unmodified instances as links.
        // Without it, instances are saved with full copies and links in files load as empty entities.
        SceneSerializer(Scene& scene, ResourceRegistry resources, PrefabLibrary* prefabs = nullptr)
            : m_Scene(scene), m_Resources(std::move(resources)), m_Prefabs(prefabs) {}

        // settings may be null: then only the entities are written / read.
        // On Load, sections missing from the file (older files) leave those settings untouched,
        // so pass in the current values. Nothing changes unless the whole file parses.
        bool Save(const std::string& path, const SceneSettings* settings = nullptr) const;
        bool Load(const std::string& path, SceneSettings* settings = nullptr);   // replaces the scene's contents

    private:
        Scene& m_Scene;
        ResourceRegistry m_Resources;
        PrefabLibrary* m_Prefabs = nullptr;
    };

} // namespace Engine
