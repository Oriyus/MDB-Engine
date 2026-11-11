#pragma once
#include "Scene/SceneSerializer.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace Engine {

    class Mesh;
    class Model;
    class Texture2D;
    class TerrainData;
    struct AnimGraph;
    struct GameDatabase;

    // The shared meshes and textures that scene and prefab files refer to by name:
    //   geometry "cube" (unit cube), "plane" (12 x 12), "sphere" (diameter 1), "model" (Models/BoomBox.glb),
    //            "model:<path>" (any model file, e.g. "model:Models/Fox.glb")
    //   texture  "checker", "texture:<path>" (any image, e.g. "texture:Textures/fence.png")
    //   texture  "checker"
    // Sphere and model are created on first use. Needs a GL context.
    class ResourceLibrary {
    public:
        // assetDir: the project's Assets (textures, models by path); engineDir: the engine's files (the sample model)
        ResourceLibrary(std::string assetDir, std::string engineDir);
        ~ResourceLibrary();
        ResourceLibrary(const ResourceLibrary&) = delete;
        ResourceLibrary& operator=(const ResourceLibrary&) = delete;

        std::shared_ptr<Mesh> Cube() const;
        std::shared_ptr<Mesh> Plane() const;
        std::shared_ptr<Mesh> Sphere();
        std::shared_ptr<Mesh> Capsule();     // "capsule": 1 wide, 2 tall
        std::shared_ptr<Mesh> Cylinder();    // "cylinder": 1 wide, 1 tall
        std::shared_ptr<Mesh> Cone();        // "cone": 1 wide base, 1 tall
        std::shared_ptr<Model> SampleModel();          // may be invalid if the file is missing
        // Any model file under the asset folder, loaded once ("Models/Fox.glb"). Null if it can't be loaded.
        // In scene files its geometry name is "model:Models/Fox.glb".
        std::shared_ptr<Model> LoadModel(const std::string& path);
        std::shared_ptr<Texture2D> Checker() const;
        // Any image under the asset folder, loaded once ("Textures/fence.png"). Null if it can't be loaded.
        // In scene files its texture name is "texture:Textures/fence.png".
        std::shared_ptr<Texture2D> LoadTexture(const std::string& path);
        // A terrain's heights and paint ("Terrains/x.terrain"), cached until the file changes. Null if missing or unreadable.
        // The data is shared: copy it before changing it (the editor does, per stroke).
        std::shared_ptr<TerrainData> LoadTerrain(const std::string& path);
        // Write it (the editor's scene save) and remember it as that file's contents
        bool SaveTerrain(const std::string& path, const std::shared_ptr<TerrainData>& data);
        // An animation graph ("AnimGraphs/x.json"), cached until the file changes. Null if missing or unreadable.
        std::shared_ptr<const AnimGraph> LoadAnimGraph(const std::string& path);
        // The editor's live copy of a graph (it edits it while the game runs): what LoadAnimGraph returns from now on
        void SetAnimGraph(const std::string& path, std::shared_ptr<const AnimGraph> graph);
        // The game database (GameDatabase::kPath), cached until the file changes; an empty one if there is none
        std::shared_ptr<const GameDatabase> LoadDatabase();
        // The editor's live copy (edited while the game runs): what LoadDatabase returns until the file changes again
        void SetDatabase(std::shared_ptr<const GameDatabase> db);

        // Name <-> resource mapping for SceneSerializer, PrefabLibrary and ScriptEngine.
        // The functions keep a pointer to this library, so it must outlive their users.
        ResourceRegistry Registry();

    private:
        std::string m_AssetDir, m_EngineDir;
        std::shared_ptr<Mesh> m_Cube, m_Plane, m_Sphere, m_Capsule, m_Cylinder, m_Cone;
        std::shared_ptr<Model> m_Model;
        std::unordered_map<std::string, std::shared_ptr<Model>> m_Models;   // by path, for "model:<path>"
        std::shared_ptr<Texture2D> m_Checker;
        std::unordered_map<std::string, std::shared_ptr<Texture2D>> m_Textures;   // by path, for "texture:<path>"
        struct CachedTerrain { std::shared_ptr<TerrainData> Data; std::filesystem::file_time_type Stamp; };
        std::unordered_map<std::string, CachedTerrain> m_Terrains;                // by path
        struct CachedGraph { std::shared_ptr<const AnimGraph> Data; std::filesystem::file_time_type Stamp; bool Live = false; };
        std::unordered_map<std::string, CachedGraph> m_Graphs;                    // by path
        std::shared_ptr<const GameDatabase> m_Database;
        std::filesystem::file_time_type m_DatabaseStamp{};
        bool m_DatabaseLive = false;
    };

} // namespace Engine
