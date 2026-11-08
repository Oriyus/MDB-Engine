#include "Runtime/ResourceLibrary.h"
#include "Renderer/Mesh.h"
#include "Renderer/Model.h"
#include "Renderer/Texture.h"

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

    std::shared_ptr<Model> ResourceLibrary::LoadModel(const std::string& path) {
        if (auto it = m_Models.find(path); it != m_Models.end()) return it->second;
        auto model = std::make_shared<Model>(m_AssetDir + path);
        if (!model->IsValid()) model = nullptr;          // remembered too: don't retry every frame
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
            for (const auto& [path, m] : m_Models) if (m && g == m) return "model:" + path;
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
            if (n.rfind("model:", 0) == 0) return LoadModel(n.substr(6));
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
        return r;
    }

} // namespace Engine
