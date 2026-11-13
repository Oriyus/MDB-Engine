#include "Runtime/ResourceLibrary.h"
#include "Renderer/Mesh.h"
#include "Renderer/Model.h"
#include "Renderer/Texture.h"
#include "Scene/Terrain.h"
#include "Animation/AnimGraph.h"
#include "Runtime/GameDatabase.h"
#include "Runtime/UIDocument.h"
#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

    namespace {
        std::shared_ptr<Texture2D> MakeChecker() {
            const uint32_t size = 256, cell = 32;
            std::vector<uint8_t> pixels(size * size * 4);
            for (uint32_t y = 0; y < size; y++) {
                for (uint32_t x = 0; x < size; x++) {
                    const bool light = ((x / cell) + (y / cell)) % 2 == 0;
                    const uint8_t c = light ? 230 : 60;
                    uint8_t* p = &pixels[(y * size + x) * 4];
                    p[0] = c; p[1] = c; p[2] = light ? 255 : c; p[3] = 255;
                }
            }
            return std::make_shared<Texture2D>(size, size, pixels.data());
        }
    }

    ResourceLibrary::ResourceLibrary(std::string assetDir, std::string engineDir)
        : m_AssetDir(std::move(assetDir)), m_EngineDir(std::move(engineDir)) {
        m_Cube = Mesh::CreateCube();
        m_Plane = Mesh::CreatePlane(12.0f, 6.0f);
        m_Checker = MakeChecker();
    }

    ResourceLibrary::~ResourceLibrary() = default;

    std::shared_ptr<Mesh> ResourceLibrary::Cube() const { return m_Cube; }
    std::shared_ptr<Mesh> ResourceLibrary::Plane() const { return m_Plane; }
    std::shared_ptr<Texture2D> ResourceLibrary::Checker() const { return m_Checker; }

    std::shared_ptr<Mesh> ResourceLibrary::Sphere() {
        if (!m_Sphere) m_Sphere = Mesh::CreateSphere(48);
        return m_Sphere;
    }

    std::shared_ptr<Mesh> ResourceLibrary::Capsule() { if (!m_Capsule) m_Capsule = Mesh::CreateCapsule(40); return m_Capsule; }
    std::shared_ptr<Mesh> ResourceLibrary::Cylinder() { if (!m_Cylinder) m_Cylinder = Mesh::CreateCylinder(40); return m_Cylinder; }
    std::shared_ptr<Mesh> ResourceLibrary::Cone() { if (!m_Cone) m_Cone = Mesh::CreateCone(40); return m_Cone; }

    std::shared_ptr<Model> ResourceLibrary::SampleModel() {
        if (!m_Model) m_Model = std::make_shared<Model>(m_EngineDir + "Models/BoomBox.glb");   // built in: "model"
        return m_Model;
    }

    std::shared_ptr<Texture2D> ResourceLibrary::LoadTexture(const std::string& path) {
        if (auto it = m_Textures.find(path); it != m_Textures.end()) return it->second;
        auto tex = std::make_shared<Texture2D>(m_AssetDir + path);
        if (tex->GetID() == 0) tex = nullptr;            // remembered too: don't retry every frame
        return m_Textures[path] = tex;
    }

    std::shared_ptr<TerrainData> ResourceLibrary::LoadTerrain(const std::string& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path file = fs::u8path(m_AssetDir + path);
        const auto stamp = fs::last_write_time(file, ec);
        if (ec) return nullptr;                           // no file (yet): the caller makes a flat terrain
        auto it = m_Terrains.find(path);
        if (it != m_Terrains.end() && it->second.Stamp == stamp) return it->second.Data;
        std::string error;
        auto data = TerrainData::Load(file.string(), &error);
        if (!data) { ENGINE_WARN("Terrain {}: {}", path, error); return nullptr; }
        m_Terrains[path] = { data, stamp };
        return data;
    }

    bool ResourceLibrary::SaveTerrain(const std::string& path, const std::shared_ptr<TerrainData>& data) {
        namespace fs = std::filesystem;
        if (!data || path.empty()) return false;
        const fs::path file = fs::u8path(m_AssetDir + path);
        if (!data->Save(file.string())) return false;
        std::error_code ec;
        m_Terrains[path] = { data, fs::last_write_time(file, ec) };
        return true;
    }

    std::shared_ptr<const AnimGraph> ResourceLibrary::LoadAnimGraph(const std::string& path) {
        namespace fs = std::filesystem;
        auto it = m_Graphs.find(path);
        if (it != m_Graphs.end() && it->second.Live) return it->second.Data;   // the editor's copy wins
        std::error_code ec;
        const fs::path file = fs::u8path(m_AssetDir + path);
        const auto stamp = fs::last_write_time(file, ec);
        if (ec) {
            if (it == m_Graphs.end()) { ENGINE_WARN("AnimGraph {}: no such file", path); m_Graphs[path] = { nullptr, {}, false }; }
            return it != m_Graphs.end() ? it->second.Data : nullptr;
        }
        if (it != m_Graphs.end() && it->second.Stamp == stamp) return it->second.Data;
        std::string error;
        auto graph = AnimGraph::Load(file.string(), &error);
        if (!graph) ENGINE_WARN("AnimGraph {}: {}", path, error);
        m_Graphs[path] = { graph, stamp, false };
        return graph;
    }

    void ResourceLibrary::SetAnimGraph(const std::string& path, std::shared_ptr<const AnimGraph> graph) {
        m_Graphs[path] = { std::move(graph), {}, true };
    }

    std::shared_ptr<const GameDatabase> ResourceLibrary::LoadDatabase() {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path file = fs::u8path(m_AssetDir + GameDatabase::kPath);
        const auto stamp = fs::last_write_time(file, ec);
        if (ec) {                                        // no file: the editor's copy, else an empty database
            if (!m_Database) m_Database = std::make_shared<GameDatabase>();
            return m_Database;
        }
        if (m_Database && stamp == m_DatabaseStamp) return m_Database;   // also the live copy, until the file is written
        std::string error;
        auto db = GameDatabase::Load(file.string(), &error);
        if (!db) {
            ENGINE_WARN("Database {}: {}", GameDatabase::kPath, error);
            if (!m_Database) m_Database = std::make_shared<GameDatabase>();
        }
        else m_Database = db;
        m_DatabaseStamp = stamp;
        m_DatabaseLive = false;
        return m_Database;
    }

    void ResourceLibrary::SetDatabase(std::shared_ptr<const GameDatabase> db) {
        if (!db) return;
        m_Database = std::move(db);
        std::error_code ec;
        m_DatabaseStamp = std::filesystem::last_write_time(std::filesystem::u8path(m_AssetDir + GameDatabase::kPath), ec);
        m_DatabaseLive = true;
    }

    std::shared_ptr<const UIDocument> ResourceLibrary::LoadUIDocument(const std::string& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path file = fs::u8path(m_AssetDir + path);
        const auto stamp = fs::last_write_time(file, ec);
        auto it = m_UIDocs.find(path);
        if (ec) {                                        // no file: the editor's copy (a new layout), else nothing
            if (it == m_UIDocs.end()) { ENGINE_WARN("UI {}: no such file", path); m_UIDocs[path] = { nullptr, {}, true }; return nullptr; }
            return it->second.Data;
        }
        if (it != m_UIDocs.end() && !it->second.Missing && it->second.Stamp == stamp) return it->second.Data;   // also the live copy
        std::string error;
        auto doc = UIDocument::Load(file.string(), &error);
        if (!doc) ENGINE_WARN("UI {}: {}", path, error);
        m_UIDocs[path] = { doc, stamp, false };
        return doc;
    }

    void ResourceLibrary::SetUIDocument(const std::string& path, std::shared_ptr<const UIDocument> doc) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(std::filesystem::u8path(m_AssetDir + path), ec);
        m_UIDocs[path] = { std::move(doc), stamp, false };   // until the file is written again
    }

    std::shared_ptr<Model> ResourceLibrary::LoadModel(const std::string& path) {
        if (auto it = m_Models.find(path); it != m_Models.end()) return it->second;
        auto model = std::make_shared<Model>(m_AssetDir + path);
        if (!model->IsValid()) model = nullptr;          // remembered too: don't retry every frame
        else {
            // A kit piece: its folder holds kit.json (see Model::NativeSize)
            std::error_code ec;
            const auto folder = std::filesystem::u8path(m_AssetDir + path).parent_path();
            model->NativeSize = std::filesystem::exists(folder / "kit.json", ec);
        }
        return m_Models[path] = model;
    }

    ResourceRegistry ResourceLibrary::Registry() {
        ResourceRegistry r;
        r.GeometryName = [this](const std::shared_ptr<Renderable>& g) -> std::string {
            if (!g) return "";
            if (g == m_Cube) return "cube";
            if (g == m_Plane) return "plane";
            if (g == m_Sphere) return "sphere";
            if (g == m_Capsule) return "capsule";
            if (g == m_Cylinder) return "cylinder";
            if (g == m_Cone) return "cone";
            if (g == m_Model) return "model";
            for (const auto& [path, m] : m_Models) {
                if (!m) continue;
                if (g == m) return "model:" + path;
                for (uint32_t k = 1; k <= m->LodCount(); k++)      // a level of detail inside the file
                    if (g == m->Lod(k)) return "model:" + path + "#lod" + std::to_string(k);
            }
            return "";
        };
        r.FindGeometry = [this](const std::string& n) -> std::shared_ptr<Renderable> {
            if (n == "cube") return m_Cube;
            if (n == "plane") return m_Plane;
            if (n == "sphere") return Sphere();
            if (n == "capsule") return Capsule();
            if (n == "cylinder") return Cylinder();
            if (n == "cone") return Cone();
            if (n == "model") return SampleModel();
            if (n.rfind("model:", 0) == 0) {
                // "model:<path>#lod<k>": level k of the levels of detail inside that file (Model::Lod)
                const size_t hash = n.rfind("#lod");
                if (hash != std::string::npos && hash + 4 < n.size() &&
                    std::all_of(n.begin() + (std::ptrdiff_t)hash + 4, n.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    auto model = LoadModel(n.substr(6, hash - 6));
                    const uint32_t k = (uint32_t)std::stoul(n.substr(hash + 4));
                    if (!model || k < 1 || k > model->LodCount()) return nullptr;
                    return model->Lod(k);
                }
                return LoadModel(n.substr(6));
            }
            return nullptr;
        };
        r.TextureName = [this](const std::shared_ptr<Texture2D>& t) -> std::string {
            if (t && t == m_Checker) return "checker";
            for (const auto& [path, tex] : m_Textures) if (tex && t == tex) return "texture:" + path;
            return "";
        };
        r.FindTexture = [this](const std::string& n) -> std::shared_ptr<Texture2D> {
            if (n == "checker") return m_Checker;
            if (n.rfind("texture:", 0) == 0) return LoadTexture(n.substr(8));
            return nullptr;
        };
        r.LoadTerrain = [this](const std::string& path) { return LoadTerrain(path); };
        r.LoadAnimGraph = [this](const std::string& path) { return LoadAnimGraph(path); };
        r.LoadDatabase = [this]() { return LoadDatabase(); };
        r.LoadUIDocument = [this](const std::string& path) { return LoadUIDocument(path); };
        return r;
    }

} // namespace Engine
