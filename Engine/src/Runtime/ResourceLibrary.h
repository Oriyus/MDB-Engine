#pragma once
#include "Scene/SceneSerializer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace Engine {

    class Mesh;
    class Model;
    class Texture2D;

    // The shared meshes and textures that scene and prefab files refer to by name:
    //   geometry "cube" (unit cube), "plane" (12 x 12), "sphere" (diameter 1), "model" (Models/BoomBox.glb),
    //            "model:<path>" (any model file, e.g. "model:Models/Fox.glb")
    //   texture  "checker", "texture:<path>" (any image, e.g. "texture:Textures/fence.png")
    //   texture  "checker"
    // Sphere and model are created on first use. Needs a GL context.
    class ResourceLibrary {
    public:
        explicit ResourceLibrary(std::string assetDir);
        ~ResourceLibrary();
        ResourceLibrary(const ResourceLibrary&) = delete;
        ResourceLibrary& operator=(const ResourceLibrary&) = delete;

        std::shared_ptr<Mesh> Cube() const;
        std::shared_ptr<Mesh> Plane() const;
        std::shared_ptr<Mesh> Sphere();
        std::shared_ptr<Model> SampleModel();          // may be invalid if the file is missing
        // Any model file under the asset folder, loaded once ("Models/Fox.glb"). Null if it can't be loaded.
        // In scene files its geometry name is "model:Models/Fox.glb".
        std::shared_ptr<Model> LoadModel(const std::string& path);
        std::shared_ptr<Texture2D> Checker() const;
        // Any image under the asset folder, loaded once ("Textures/fence.png"). Null if it can't be loaded.
        // In scene files its texture name is "texture:Textures/fence.png".
        std::shared_ptr<Texture2D> LoadTexture(const std::string& path);

        // Name <-> resource mapping for SceneSerializer, PrefabLibrary and ScriptEngine.
        // The functions keep a pointer to this library, so it must outlive their users.
        ResourceRegistry Registry();

    private:
        std::string m_AssetDir;
        std::shared_ptr<Mesh> m_Cube, m_Plane, m_Sphere;
        std::shared_ptr<Model> m_Model;
        std::unordered_map<std::string, std::shared_ptr<Model>> m_Models;   // by path, for "model:<path>"
        std::shared_ptr<Texture2D> m_Checker;
        std::unordered_map<std::string, std::shared_ptr<Texture2D>> m_Textures;   // by path, for "texture:<path>"
    };

} // namespace Engine
