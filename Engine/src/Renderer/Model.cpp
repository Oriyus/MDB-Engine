#include "Renderer/Model.h"
#include "Core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Engine {

    Model::Model(const std::string& path) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path,
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType |
            aiProcess_PreTransformVertices);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0) {
            ENGINE_ERROR("Failed to load model '{}': {}", path, importer.GetErrorString());
            return;
        }

        const fs::path baseDir = fs::path(path).parent_path();
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        // glTF stores colors in linear space. Our Material.Color is sRGB-style, so convert.
        const bool linearColors = (ext == ".gltf" || ext == ".glb");

        // ---- textures (each file is loaded once, even if several materials use it)
        std::unordered_map<std::string, std::shared_ptr<Texture2D>> cache;
        auto loadTexture = [&](const aiString& texPath) -> std::shared_ptr<Texture2D> {
            const std::string key = texPath.C_Str();
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;

            std::shared_ptr<Texture2D> tex;
            if (const aiTexture* emb = scene->GetEmbeddedTexture(key.c_str())) {
                if (emb->mHeight == 0)   // compressed image (png/jpg) stored in the file
                    tex = Texture2D::FromMemory(reinterpret_cast<const uint8_t*>(emb->pcData), emb->mWidth, true);
                else
                    ENGINE_WARN("Raw embedded texture '{}' is not supported", key);
            }
            else {
                std::string rel = key;
                std::replace(rel.begin(), rel.end(), '\\', '/');   // MTL files from Windows tools
                fs::path p = baseDir / rel;
                if (!fs::exists(p)) p = baseDir / fs::path(rel).filename();   // flat folder fallback
                if (fs::exists(p)) tex = std::make_shared<Texture2D>(p.string(), true);
                else ENGINE_WARN("Texture not found for model '{}': {}", path, key);
            }
            cache[key] = tex;
            return tex;
            };

        // ---- materials
        for (unsigned i = 0; i < scene->mNumMaterials; i++) {
            const aiMaterial* am = scene->mMaterials[i];
            Material m;

            aiString name;
            if (am->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) m.Name = name.C_Str();

            aiColor4D c;
            if (am->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS ||
                am->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS) {
                m.Color = glm::vec3(c.r, c.g, c.b);
                if (linearColors) m.Color = glm::pow(m.Color, glm::vec3(1.0f / 2.2f));
            }

            aiString texPath;
            if (am->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                am->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                m.Albedo = loadTexture(texPath);

            float shininess;
            if (am->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
                m.Shininess = std::clamp(shininess, 2.0f, 256.0f);

            aiColor3D spec;
            if (am->Get(AI_MATKEY_COLOR_SPECULAR, spec) == AI_SUCCESS)
                m.SpecularStrength = std::clamp(std::max({ spec.r, spec.g, spec.b }), 0.0f, 1.0f);

            m_Materials.push_back(std::move(m));
        }
        if (m_Materials.empty()) m_Materials.emplace_back();   // always at least one

        // ---- meshes
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);

        for (unsigned i = 0; i < scene->mNumMeshes; i++) {
            const aiMesh* m = scene->mMeshes[i];

            std::vector<Vertex> vertices;
            vertices.reserve(m->mNumVertices);
            for (unsigned v = 0; v < m->mNumVertices; v++) {
                Vertex vert;
                vert.Position = glm::vec3(m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z);
                if (m->HasNormals())
                    vert.Normal = glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z);
                if (m->HasTextureCoords(0))
                    vert.TexCoord = glm::vec2(m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y);
                mn = glm::min(mn, vert.Position);
                mx = glm::max(mx, vert.Position);
                vertices.push_back(vert);
            }

            std::vector<uint32_t> indices;
            indices.reserve(m->mNumFaces * 3);
            for (unsigned f = 0; f < m->mNumFaces; f++) {
                const aiFace& face = m->mFaces[f];
                if (face.mNumIndices != 3) continue;   // skip stray lines/points
                indices.push_back(face.mIndices[0]);
                indices.push_back(face.mIndices[1]);
                indices.push_back(face.mIndices[2]);
            }

            if (vertices.empty() || indices.empty()) continue;

            Submesh sub;
            sub.Geometry = std::make_unique<Mesh>(vertices, indices);
            sub.MaterialIndex = std::min<uint32_t>(m->mMaterialIndex, (uint32_t)m_Materials.size() - 1);
            m_Submeshes.push_back(std::move(sub));
        }

        m_Min = mn;
        m_Max = mx;
        ENGINE_INFO("Loaded model '{}': {} submesh(es), {} material(s)", path, m_Submeshes.size(), m_Materials.size());
    }

    void Model::Draw() const {
        for (const auto& s : m_Submeshes) s.Geometry->Draw();
    }

    void Model::DrawSubmesh(uint32_t i) const {
        if (i < m_Submeshes.size()) m_Submeshes[i].Geometry->Draw();
    }

    const Material* Model::GetSubmeshMaterial(uint32_t i) const {
        return i < m_Submeshes.size() ? &m_Materials[m_Submeshes[i].MaterialIndex] : nullptr;
    }

    bool Model::HasTexturedMaterials() const {
        for (const auto& m : m_Materials)
            if (m.Albedo) return true;
        return false;
    }

    glm::mat4 Model::GetNormalizeMatrix(float targetSize) const {
        glm::vec3 size = m_Max - m_Min;
        float extent = std::max({ size.x, size.y, size.z });
        float s = extent > 0.0f ? targetSize / extent : 1.0f;
        glm::vec3 center = (m_Min + m_Max) * 0.5f;
        return glm::scale(glm::mat4(1.0f), glm::vec3(s)) * glm::translate(glm::mat4(1.0f), -center);
    }

} // namespace Engine