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

    ResourceLibrary::ResourceLibrary(std::string assetDir) : m_AssetDir(std::move(assetDir)) {
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

    std::shared_ptr<Model> ResourceLibrary::SampleModel() {
        if (!m_Model) m_Model = std::make_shared<Model>(m_AssetDir + "Models/BoomBox.glb");
        return m_Model;
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
            if (g == m_Model) return "model";
            for (const auto& [path, m] : m_Models) if (m && g == m) return "model:" + path;
            return "";
        };
        r.FindGeometry = [this](const std::string& n) -> std::shared_ptr<Renderable> {
            if (n == "cube") return m_Cube;
            if (n == "plane") return m_Plane;
            if (n == "sphere") return Sphere();
            if (n == "model") return SampleModel();
            if (n.rfind("model:", 0) == 0) return LoadModel(n.substr(6));
            return nullptr;
        };
        r.TextureName = [this](const std::shared_ptr<Texture2D>& t) -> std::string {
            return t && t == m_Checker ? "checker" : "";
        };
        r.FindTexture = [this](const std::string& n) -> std::shared_ptr<Texture2D> {
            return n == "checker" ? m_Checker : nullptr;
        };
        return r;
    }

} // namespace Engine
