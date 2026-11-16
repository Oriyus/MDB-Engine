// ResourceLibrary: material files (Renderer/MaterialAsset.h) and the data textures they use
#include "Runtime/ResourceLibrary.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/Texture.h"
#include "Core/Log.h"

#include <stb_image.h>      // implementation is compiled in Texture.cpp
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace Engine {

    namespace {
        // An image under Assets as RGBA rows, bottom row first (like Texture2D). False if it can't be read.
        bool ReadImage(const std::string& file, int& w, int& h, std::vector<uint8_t>& px) {
            stbi_set_flip_vertically_on_load(true);
            int channels = 0;
            uint8_t* data = stbi_load(file.c_str(), &w, &h, &channels, 4);
            if (!data) return false;
            px.assign(data, data + size_t(w) * h * 4);
            stbi_image_free(data);
            return true;
        }
    }

    std::shared_ptr<Texture2D> ResourceLibrary::LoadDataTexture(const std::string& path, bool flipGreen) {
        if (path.empty()) return nullptr;
        const std::string key = path + (flipGreen ? "|flipY" : "");
        if (auto it = m_DataTextures.find(key); it != m_DataTextures.end()) return it->second;
        std::shared_ptr<Texture2D> tex;
        int w = 0, h = 0;
        std::vector<uint8_t> px;
        if (ReadImage(m_AssetDir + path, w, h, px)) {
            if (flipGreen)                                // DirectX style: green points down the image
                for (size_t i = 1; i < px.size(); i += 4) px[i] = (uint8_t)(255 - px[i]);
            tex = std::make_shared<Texture2D>((uint32_t)w, (uint32_t)h, px.data(), false);
            tex->DropPixels();                            // data, never alpha-tested
        }
        else ENGINE_WARN("Material map {}: can't read it ({})", path, stbi_failure_reason());
        return m_DataTextures[key] = tex;
    }

    std::shared_ptr<Texture2D> ResourceLibrary::PackMetalRough(const std::string& roughness, const std::string& metallic) {
        if (roughness.empty() && metallic.empty()) return nullptr;
        const std::string key = "pack|" + roughness + "|" + metallic;
        if (auto it = m_DataTextures.find(key); it != m_DataTextures.end()) return it->second;
        int w[2] = {}, h[2] = {};
        std::vector<uint8_t> px[2];
        bool have[2] = {};
        const std::string* paths[2] = { &roughness, &metallic };
        int W = 0, H = 0;
        for (int i = 0; i < 2; i++) {
            if (paths[i]->empty()) continue;
            have[i] = ReadImage(m_AssetDir + *paths[i], w[i], h[i], px[i]);
            if (!have[i]) { ENGINE_WARN("Material map {}: can't read it ({})", *paths[i], stbi_failure_reason()); continue; }
            W = std::max(W, w[i]); H = std::max(H, h[i]);
        }
        std::shared_ptr<Texture2D> tex;
        if (W > 0 && H > 0) {
            // glTF layout: G roughness, B metallic; a missing map is 1 (the material's factor alone)
            std::vector<uint8_t> out(size_t(W) * H * 4, 255);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    uint8_t* p = &out[(size_t(y) * W + x) * 4];
                    for (int i = 0; i < 2; i++) {
                        if (!have[i]) continue;
                        const int sx = x * w[i] / W, sy = y * h[i] / H;     // nearest, if the sizes differ
                        p[1 + i] = px[i][(size_t(sy) * w[i] + sx) * 4];
                    }
                }
            tex = std::make_shared<Texture2D>((uint32_t)W, (uint32_t)H, out.data(), false);
            tex->DropPixels();
        }
        return m_DataTextures[key] = tex;
    }

    std::shared_ptr<MaterialAsset> ResourceLibrary::LoadMaterial(const std::string& path) {
        if (path.empty()) return nullptr;
        if (auto it = m_Materials.find(path); it != m_Materials.end()) return it->second.Asset;
        auto m = std::make_shared<MaterialAsset>();
        m->Path = path;
        CachedMaterial& c = m_Materials[path];
        c.Asset = m;
        ReadMaterialFile(c);
        std::vector<MaterialAsset*> resolving;
        std::vector<const MaterialAsset*> done;
        ResolveMaterialTree(*m, resolving, done);
        return m;
    }

    void ResourceLibrary::ReadMaterialFile(CachedMaterial& c) {
        namespace fs = std::filesystem;
        MaterialAsset& m = *c.Asset;
        std::error_code ec;
        const fs::path file = fs::u8path(m_AssetDir + m.Path);
        c.Stamp = fs::last_write_time(file, ec);
        m.Missing = (bool)ec;
        m.File = json::object();
        c.ReadError.clear();
        if (m.Missing) { ENGINE_WARN("Material {}: no such file", m.Path); return; }
        try {
            std::ifstream in(file, std::ios::binary);
            json j = json::parse(in);
            if (!j.is_object()) throw std::runtime_error("not a JSON object");
            m.File = std::move(j);
        }
        catch (const std::exception& e) {
            c.ReadError = e.what();
            ENGINE_WARN("Material {}: {}", m.Path, c.ReadError);
        }
    }

    // A material after its parent. resolving: the chain being worked on (meeting one again is a loop); done: finished ones.
    void ResourceLibrary::ResolveMaterialTree(MaterialAsset& m, std::vector<MaterialAsset*>& resolving, std::vector<const MaterialAsset*>& done) {
        if (std::find(done.begin(), done.end(), &m) != done.end()) return;
        resolving.push_back(&m);
        const auto cached = m_Materials.find(m.Path);
        m.Error = cached != m_Materials.end() ? cached->second.ReadError : std::string();
        json merged = json::object();
        const std::string parentPath = m.Parent();
        if (!parentPath.empty()) {
            std::shared_ptr<MaterialAsset> parent = LoadMaterial(parentPath);   // a new one is resolved right away
            if (!parent || parent.get() == &m || std::find(resolving.begin(), resolving.end(), parent.get()) != resolving.end())
                m.Error = "its parent " + parentPath + " leads back to it";
            else {
                ResolveMaterialTree(*parent, resolving, done);
                if (parent->Missing) m.Error = "no parent " + parentPath;
                merged = MaterialDescToJson(parent->Desc);
            }
        }
        for (auto it = m.File.begin(); it != m.File.end(); ++it)
            if (it.key() != "version" && it.key() != "parent") merged[it.key()] = it.value();
        m.Desc = MaterialDescFromJson(merged);
        if (m.Desc.Shader != "standard" && m.Error.empty())
            m.Error = "unknown shader \"" + m.Desc.Shader + "\" (drawn with standard)";

        // The textures, for mesh parts
        const MaterialDesc& d = m.Desc;
        Material& s = m.Surface;
        s = Material{};
        s.Name = std::filesystem::u8path(m.Path).stem().string();
        s.Color = m.Missing ? glm::vec3(1.0f, 0.0f, 1.0f) : d.Color;     // magenta: the file is missing
        s.Albedo = d.AlbedoMap.empty() ? nullptr : d.AlbedoMap == "checker" ? m_Checker : LoadTexture(d.AlbedoMap);   // "checker": built in
        s.Normal = LoadDataTexture(d.NormalMap, d.NormalFlipY);
        s.MetalRough = !d.MetalRoughMap.empty() ? LoadDataTexture(d.MetalRoughMap) : PackMetalRough(d.RoughnessMap, d.MetallicMap);
        s.Emissive = d.EmissiveMap.empty() ? nullptr : LoadTexture(d.EmissiveMap);
        s.Metallic = d.Metallic;
        s.Roughness = d.Roughness;
        s.EmissiveColor = d.Emissive * d.EmissiveIntensity;
        s.Alpha = d.Alpha;
        s.Mode = d.Mode;
        s.AlphaCutoff = d.AlphaCutoff;
        s.DoubleSided = d.DoubleSided;
        s.Specular = d.Specular;
        s.NormalStrength = d.NormalStrength;
        s.UVScale = d.UVScale;
        s.UVOffset = d.UVOffset;
        s.Unlit = d.Unlit;
        s.CastShadows = d.CastShadows;

        // The same maps packed the way terrain layers read them (normal, grey roughness, height)
        TerrainLayer layer;
        layer.NormalMap = d.NormalMap;
        layer.RoughnessMap = d.RoughnessMap;
        layer.HeightMap = d.HeightMap;
        layer.NormalFlipY = d.NormalFlipY;
        m.TerrainMaps = LoadTerrainSurface(layer);

        m.Revision++;
        resolving.pop_back();
        done.push_back(&m);
    }

    void ResourceLibrary::ResolveMaterials() {
        std::vector<MaterialAsset*> resolving;
        std::vector<const MaterialAsset*> done;
        std::vector<std::shared_ptr<MaterialAsset>> all;          // LoadMaterial may add entries (new parents) meanwhile
        for (auto& [path, c] : m_Materials) all.push_back(c.Asset);
        for (auto& m : all) ResolveMaterialTree(*m, resolving, done);
    }

    bool ResourceLibrary::RefreshMaterials(const std::function<bool(const MaterialAsset&)>& keep) {
        namespace fs = std::filesystem;
        bool changed = false;
        for (auto& [path, c] : m_Materials) {
            if (c.Virtual || (keep && keep(*c.Asset))) continue;   // in memory only, or held by the editor
            std::error_code ec;
            const auto stamp = fs::last_write_time(fs::u8path(m_AssetDir + path), ec);
            const bool missing = (bool)ec;
            if (missing == c.Asset->Missing && (missing || stamp == c.Stamp)) continue;
            ReadMaterialFile(c);
            changed = true;
        }
        if (changed) ResolveMaterials();
        return changed;
    }

    void ResourceLibrary::SetMaterialFile(const std::string& path, const json& contents) {
        if (!LoadMaterial(path)) return;
        CachedMaterial& c = m_Materials[path];
        c.Asset->File = contents.is_object() ? contents : json::object();
        c.ReadError.clear();
        ResolveMaterials();
    }

    void ResourceLibrary::ReloadMaterial(const std::string& path) {
        auto it = m_Materials.find(path);
        if (it == m_Materials.end() || it->second.Virtual) return;
        ReadMaterialFile(it->second);
        ResolveMaterials();
    }

    bool ResourceLibrary::SaveMaterial(const std::string& path, const json& contents, std::string* error) {
        namespace fs = std::filesystem;
        if (path.empty()) { if (error) *error = "no path"; return false; }
        const fs::path file = fs::u8path(m_AssetDir + path);
        json out = json::object();
        out["version"] = 1;                              // (written in key order, like every JSON file here)
        if (contents.is_object()) {
            if (auto p = contents.find("parent"); p != contents.end() && p->is_string() && !p->get<std::string>().empty()) out["parent"] = *p;
            for (auto it = contents.begin(); it != contents.end(); ++it)
                if (it.key() != "version" && it.key() != "parent") out[it.key()] = it.value();
        }
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        const fs::path temp = fs::u8path(m_AssetDir + path + ".tmp");
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            if (f) f << out.dump(2) << "\n";
            if (!f) { if (error) *error = "can't write " + path + ".tmp"; return false; }
        }
        fs::rename(temp, file, ec);
        if (ec) { fs::remove(temp, ec); if (error) *error = "can't replace " + path; return false; }

        auto it = m_Materials.find(path);
        if (it == m_Materials.end()) { LoadMaterial(path); return true; }
        CachedMaterial& c = it->second;
        c.Asset->File = out;
        c.Asset->Missing = false;
        c.Stamp = fs::last_write_time(file, ec);
        c.ReadError.clear();
        ResolveMaterials();
        return true;
    }

    std::vector<std::shared_ptr<MaterialAsset>> ResourceLibrary::LoadedMaterials() const {
        std::vector<std::shared_ptr<MaterialAsset>> out;
        for (const auto& [path, c] : m_Materials) out.push_back(c.Asset);
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a->Path < b->Path; });
        return out;
    }

} // namespace Engine

namespace Engine {

    std::string ResourceLibrary::MaterialForLook(const std::string& nameHint, const MaterialDesc& look) {
        namespace fs = std::filesystem;
        static const MaterialDesc def;
        json values = MaterialDescToJson(look, &def);     // only what differs from the defaults
        values["shader"] = look.Shader;
        const std::string key = values.dump();
        const std::string folder = "Materials/Generated/";

        // The generated files already there, by their values (once; new ones are added as they are made)
        if (!m_LooksIndexed) {
            m_LooksIndexed = true;
            std::error_code ec;
            for (auto it = fs::directory_iterator(fs::u8path(m_AssetDir + folder), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->path().extension() != ".mat") continue;
                try {
                    std::ifstream in(it->path(), std::ios::binary);
                    json j = json::parse(in);
                    if (!j.is_object() || j.contains("parent")) continue;
                    const MaterialDesc d = MaterialDescFromJson(j);
                    json v = MaterialDescToJson(d, &def);
                    v["shader"] = d.Shader;
                    m_Looks.emplace(v.dump(), folder + it->path().filename().string());
                }
                catch (const std::exception&) {}
            }
        }
        if (auto it = m_Looks.find(key); it != m_Looks.end()) return it->second;

        // A new file, named after the first object with this look
        std::string base;
        std::string hint = nameHint;
        if (const size_t dot = hint.find(".json"); dot != std::string::npos) hint.erase(dot, 5);   // "Exit: x.json > y"
        for (char c : hint) {
            const char k = (std::isalnum((unsigned char)c) || c == '-') ? c : '_';
            if (k != '_' || (!base.empty() && base.back() != '_')) base += k;   // no runs of _
        }
        while (!base.empty() && base.back() == '_') base.pop_back();
        if (base.empty()) base = "Look";
        std::string path;
        for (int n = 1; n < 10000; n++) {
            path = folder + base + (n > 1 ? "_" + std::to_string(n) : "") + ".mat";
            std::error_code ec;
            if (!m_Materials.count(path) && !fs::exists(fs::u8path(m_AssetDir + path), ec)) break;
        }
        m_Looks[key] = path;
        if (m_WriteGenerated) {
            std::string error;
            if (!SaveMaterial(path, values, &error)) { ENGINE_WARN("Material {}: {}", path, error); m_Looks.erase(key); return ""; }
            ENGINE_INFO("Made {} from an older Mesh Renderer look", path);
            return path;
        }
        // In memory only: a material that never reads its (absent) file
        auto m = std::make_shared<MaterialAsset>();
        m->Path = path;
        m->File = values;
        CachedMaterial& c = m_Materials[path];
        c.Asset = m;
        c.Virtual = true;
        ResolveMaterials();
        return path;
    }

} // namespace Engine
