#include "Renderer/Model.h"
#include "Core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
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
        // Read without flattening first: a model with bones keeps its node tree for animation.
        // Anything else is flattened afterwards (PreTransformVertices), as before.
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path,
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType |
            aiProcess_LimitBoneWeights);          // at most 4 bones per vertex

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0) {
            ENGINE_ERROR("Failed to load model '{}': {}", path, importer.GetErrorString());
            return;
        }
        bool skinned = false;
        for (unsigned i = 0; i < scene->mNumMeshes; i++) skinned = skinned || scene->mMeshes[i]->HasBones();
        if (!skinned) scene = importer.ApplyPostProcessing(aiProcess_PreTransformVertices);
        if (!scene) {
            ENGINE_ERROR("Failed to load model '{}': {}", path, importer.GetErrorString());
            return;
        }

        const fs::path baseDir = fs::path(path).parent_path();
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        // glTF stores colors in linear space. Our Material.Color is sRGB-style, so convert.
        const bool linearColors = (ext == ".gltf" || ext == ".glb");

        std::unordered_map<std::string, std::shared_ptr<Texture2D>> cache;
        auto loadTexture = [&](const aiString& texPath, bool srgb) -> std::shared_ptr<Texture2D> {
            const std::string file = texPath.C_Str();
            const std::string key = file + (srgb ? "|srgb" : "|linear");   // same file, different decode
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;

            std::shared_ptr<Texture2D> tex;
            if (const aiTexture* emb = scene->GetEmbeddedTexture(file.c_str())) {
                if (emb->mHeight == 0)
                    tex = Texture2D::FromMemory(reinterpret_cast<const uint8_t*>(emb->pcData), emb->mWidth, srgb);
                else
                    ENGINE_WARN("Raw embedded texture '{}' is not supported", file);
            }
            else {
                std::string rel = file;
                std::replace(rel.begin(), rel.end(), '\\', '/');
                fs::path p = baseDir / rel;
                if (!fs::exists(p)) p = baseDir / fs::path(rel).filename();
                if (fs::exists(p)) tex = std::make_shared<Texture2D>(p.string(), srgb);
                else ENGINE_WARN("Texture not found for model '{}': {}", path, file);
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
                m.Alpha = std::clamp(c.a, 0.0f, 1.0f);
            }

            // Transparency: glTF says how alpha is meant (alphaMode); other formats only have an opacity
            aiString alphaMode;
            if (am->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
                const std::string mode = alphaMode.C_Str();
                m.Mode = mode == "BLEND" ? AlphaMode::Blend : mode == "MASK" ? AlphaMode::Cutout : AlphaMode::Opaque;
                float cutoff;
                if (am->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS) m.AlphaCutoff = cutoff;
            }
            else {
                float opacity;
                if (am->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.999f && opacity > 0.0f) m.Alpha = opacity;
                if (m.Alpha < 0.999f) m.Mode = AlphaMode::Blend;
            }

            aiString tp;
            if (am->GetTexture(aiTextureType_BASE_COLOR, 0, &tp) == AI_SUCCESS ||
                am->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS)
                m.Albedo = loadTexture(tp, true);

            if (am->GetTexture(aiTextureType_NORMALS, 0, &tp) == AI_SUCCESS)
                m.Normal = loadTexture(tp, false);

            // Only the glTF layout (G = roughness, B = metallic) is understood
            if (linearColors &&
                (am->GetTexture(aiTextureType_UNKNOWN, 0, &tp) == AI_SUCCESS ||
                    am->GetTexture(aiTextureType_METALNESS, 0, &tp) == AI_SUCCESS))
                m.MetalRough = loadTexture(tp, false);

            if (am->GetTexture(aiTextureType_EMISSIVE, 0, &tp) == AI_SUCCESS)
                m.Emissive = loadTexture(tp, true);

            // Factors. OBJ files have no PBR values, so convert the old shininess to a roughness.
            m.Metallic = 0.0f;
            m.Roughness = 0.6f;
            float f;
            if (am->Get(AI_MATKEY_METALLIC_FACTOR, f) == AI_SUCCESS) m.Metallic = f;
            if (am->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) == AI_SUCCESS)
                m.Roughness = f;
            else if (am->Get(AI_MATKEY_SHININESS, f) == AI_SUCCESS)
                m.Roughness = std::clamp(std::sqrt(2.0f / (f + 2.0f)), 0.05f, 1.0f);

            aiColor3D em;
            if (am->Get(AI_MATKEY_COLOR_EMISSIVE, em) == AI_SUCCESS)
                m.EmissiveColor = glm::vec3(em.r, em.g, em.b);

            m_Materials.push_back(std::move(m));
        }
        if (m_Materials.empty()) m_Materials.emplace_back();   // always at least one

        if (skinned) {
            LoadSkinned(scene);
            ENGINE_INFO("Loaded animated model '{}': {} submesh(es), {} bone(s), {} animation(s)", path,
                m_Submeshes.size(), m_Bones.size(), m_Clips.size());
            return;
        }

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

    void Model::DrawInstanced(uint32_t count) const {
        for (const auto& s : m_Submeshes) s.Geometry->DrawInstanced(count);
    }

    void Model::DrawSubmeshInstanced(uint32_t i, uint32_t count) const {
        if (i < m_Submeshes.size()) m_Submeshes[i].Geometry->DrawInstanced(count);
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

    // ---------------------------------------------------------------- skeletal animation
    namespace {
        glm::mat4 ToGlm(const aiMatrix4x4& m) {       // assimp is row-major
            return glm::transpose(glm::mat4(m.a1, m.a2, m.a3, m.a4, m.b1, m.b2, m.b3, m.b4,
                m.c1, m.c2, m.c3, m.c4, m.d1, m.d2, m.d3, m.d4));
        }

        Model::NodePose ToPose(const glm::mat4& m) {
            Model::NodePose p;
            p.T = glm::vec3(m[3]);
            p.S = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            const glm::vec3 s = glm::max(p.S, glm::vec3(1e-8f));
            p.R = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / s.x, glm::vec3(m[1]) / s.y, glm::vec3(m[2]) / s.z)));
            return p;
        }

        glm::mat4 ToMatrix(const Model::NodePose& p) {
            return glm::translate(glm::mat4(1.0f), p.T) * glm::mat4_cast(p.R) * glm::scale(glm::mat4(1.0f), p.S);
        }

        // Linear search is fine: clips have tens of keys per channel
        template<typename T, typename Lerp>
        T SampleKeys(const std::vector<std::pair<float, T>>& keys, float t, const T& fallback, Lerp lerp) {
            if (keys.empty()) return fallback;
            if (t <= keys.front().first) return keys.front().second;
            if (t >= keys.back().first) return keys.back().second;
            for (size_t i = 0; i + 1 < keys.size(); i++) {
                if (t < keys[i + 1].first) {
                    const float span = keys[i + 1].first - keys[i].first;
                    const float a = span > 0.0f ? (t - keys[i].first) / span : 0.0f;
                    return lerp(keys[i].second, keys[i + 1].second, a);
                }
            }
            return keys.back().second;
        }
    }

    void Model::LoadSkinned(const aiScene* scene) {
        // ---- the node tree, parents before children
        std::unordered_map<std::string, int> nodeIndex;
        std::vector<std::pair<const aiNode*, int>> stack{ { scene->mRootNode, -1 } };
        std::vector<std::pair<const aiNode*, int>> meshNodes;    // (node, its index) for rigid meshes
        while (!stack.empty()) {
            auto [node, parent] = stack.back();
            stack.pop_back();
            const int idx = (int)m_Nodes.size();
            m_Nodes.push_back({ node->mName.C_Str(), parent, ToPose(ToGlm(node->mTransformation)) });
            nodeIndex.emplace(node->mName.C_Str(), idx);
            if (node->mNumMeshes > 0) meshNodes.emplace_back(node, idx);
            for (int c = (int)node->mNumChildren - 1; c >= 0; c--) stack.emplace_back(node->mChildren[c], idx);
        }

        // Bones are skeleton nodes with an offset matrix; the same node is one bone for every mesh
        std::unordered_map<int, int> boneOfNode;
        auto boneFor = [&](int node, const glm::mat4& offset) {
            if (auto it = boneOfNode.find(node); it != boneOfNode.end()) return it->second;
            const int b = (int)m_Bones.size();
            m_Bones.push_back({ node, offset });
            boneOfNode[node] = b;
            return b;
        };

        // ---- vertices (kept until the rest pose is known, for bounds and colliders)
        struct Pending { std::vector<SkinnedVertex> Vertices; std::vector<uint32_t> Indices; uint32_t Material; };
        std::vector<Pending> pending;
        for (unsigned i = 0; i < scene->mNumMeshes; i++) {
            const aiMesh* m = scene->mMeshes[i];
            Pending p;
            p.Material = std::min<uint32_t>(m->mMaterialIndex, (uint32_t)m_Materials.size() - 1);
            p.Vertices.resize(m->mNumVertices);
            for (unsigned v = 0; v < m->mNumVertices; v++) {
                SkinnedVertex& sv = p.Vertices[v];
                sv.Position = glm::vec3(m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z);
                if (m->HasNormals()) sv.Normal = glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z);
                if (m->HasTextureCoords(0)) sv.TexCoord = glm::vec2(m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y);
            }
            if (m->HasBones()) {
                for (unsigned bi = 0; bi < m->mNumBones; bi++) {
                    const aiBone* bone = m->mBones[bi];
                    auto it = nodeIndex.find(bone->mName.C_Str());
                    if (it == nodeIndex.end()) continue;
                    const int b = boneFor(it->second, ToGlm(bone->mOffsetMatrix));
                    for (unsigned w = 0; w < bone->mNumWeights; w++) {
                        SkinnedVertex& sv = p.Vertices[bone->mWeights[w].mVertexId];
                        for (int k = 0; k < 4; k++)                     // the first free slot
                            if (sv.Weights[k] == 0.0f) { sv.Bones[k] = b; sv.Weights[k] = bone->mWeights[w].mWeight; break; }
                    }
                }
            }
            else {
                // A rigid part of an animated model: it follows the node that holds it
                int node = 0;
                for (auto& [n, idx] : meshNodes)
                    for (unsigned k = 0; k < n->mNumMeshes; k++) if (n->mMeshes[k] == i) node = idx;
                const int b = boneFor(node, glm::mat4(1.0f));
                for (auto& sv : p.Vertices) { sv.Bones = glm::ivec4(b, 0, 0, 0); sv.Weights = glm::vec4(1, 0, 0, 0); }
            }
            for (auto& sv : p.Vertices) {                              // weights sum to 1
                const float sum = sv.Weights.x + sv.Weights.y + sv.Weights.z + sv.Weights.w;
                if (sum > 0.0f) sv.Weights /= sum;
                else { sv.Bones = glm::ivec4(0); sv.Weights = glm::vec4(1, 0, 0, 0); }
            }
            for (unsigned f = 0; f < m->mNumFaces; f++) {
                const aiFace& face = m->mFaces[f];
                if (face.mNumIndices != 3) continue;
                p.Indices.insert(p.Indices.end(), { face.mIndices[0], face.mIndices[1], face.mIndices[2] });
            }
            if (!p.Vertices.empty() && !p.Indices.empty()) pending.push_back(std::move(p));
        }

        // ---- animation clips (times in seconds)
        for (unsigned a = 0; a < scene->mNumAnimations; a++) {
            const aiAnimation* anim = scene->mAnimations[a];
            const double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
            Clip clip;
            clip.Duration = (float)(anim->mDuration / tps);
            for (unsigned c = 0; c < anim->mNumChannels; c++) {
                const aiNodeAnim* ch = anim->mChannels[c];
                auto it = nodeIndex.find(ch->mNodeName.C_Str());
                if (it == nodeIndex.end()) continue;
                Channel out;
                out.Node = it->second;
                for (unsigned k = 0; k < ch->mNumPositionKeys; k++) {
                    const auto& key = ch->mPositionKeys[k];
                    out.T.emplace_back((float)(key.mTime / tps), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z));
                }
                for (unsigned k = 0; k < ch->mNumRotationKeys; k++) {
                    const auto& key = ch->mRotationKeys[k];
                    out.R.emplace_back((float)(key.mTime / tps), glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z));
                }
                for (unsigned k = 0; k < ch->mNumScalingKeys; k++) {
                    const auto& key = ch->mScalingKeys[k];
                    out.S.emplace_back((float)(key.mTime / tps), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z));
                }
                clip.Channels.push_back(std::move(out));
            }
            std::string name = anim->mName.C_Str();
            if (name.empty()) name = "Animation " + std::to_string(a);
            m_ClipNames.push_back(name);
            m_Clips.push_back(std::move(clip));
        }

        // ---- the rest pose: where vertices are without animation (bounds, colliders)
        std::vector<NodePose> rest;
        SamplePose(-1, 0.0f, rest);
        std::vector<glm::mat4> palette;
        PoseToPalette(rest, palette);
        m_RestPalette = palette;
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (auto& p : pending) {
            std::vector<glm::vec3> restPositions;
            restPositions.reserve(p.Vertices.size());
            for (const auto& sv : p.Vertices) {
                glm::mat4 skin(0.0f);
                for (int k = 0; k < 4; k++) skin += palette[sv.Bones[k]] * sv.Weights[k];
                const glm::vec3 rp = glm::vec3(skin * glm::vec4(sv.Position, 1.0f));
                restPositions.push_back(rp);
                mn = glm::min(mn, rp);
                mx = glm::max(mx, rp);
            }
            Submesh sub;
            sub.Geometry = std::make_unique<Mesh>(p.Vertices, p.Indices, restPositions);
            sub.MaterialIndex = p.Material;
            m_Submeshes.push_back(std::move(sub));
        }
        m_Min = mn;
        m_Max = mx;
    }

    int Model::FindAnimation(const std::string& name) const {
        for (size_t i = 0; i < m_ClipNames.size(); i++) if (m_ClipNames[i] == name) return (int)i;
        return -1;
    }

    float Model::AnimationDuration(int clip) const {
        return clip >= 0 && clip < (int)m_Clips.size() ? m_Clips[clip].Duration : 0.0f;
    }

    void Model::SamplePose(int clip, float time, std::vector<NodePose>& pose) const {
        pose.resize(m_Nodes.size());
        for (size_t i = 0; i < m_Nodes.size(); i++) pose[i] = m_Nodes[i].Rest;
        if (clip < 0 || clip >= (int)m_Clips.size()) return;
        auto mixV = [](const glm::vec3& a, const glm::vec3& b, float t) { return glm::mix(a, b, t); };
        auto mixQ = [](const glm::quat& a, const glm::quat& b, float t) { return glm::slerp(a, b, t); };
        for (const auto& ch : m_Clips[clip].Channels) {
            NodePose& p = pose[ch.Node];
            p.T = SampleKeys(ch.T, time, p.T, mixV);
            p.R = glm::normalize(SampleKeys(ch.R, time, p.R, mixQ));
            p.S = SampleKeys(ch.S, time, p.S, mixV);
        }
    }

    void Model::PoseToPalette(const std::vector<NodePose>& pose, std::vector<glm::mat4>& palette) const {
        std::vector<glm::mat4> global(m_Nodes.size());
        for (size_t i = 0; i < m_Nodes.size(); i++) {           // parents come first
            const glm::mat4 local = ToMatrix(i < pose.size() ? pose[i] : m_Nodes[i].Rest);
            global[i] = m_Nodes[i].Parent >= 0 ? global[m_Nodes[i].Parent] * local : local;
        }
        palette.resize(m_Bones.size());
        for (size_t b = 0; b < m_Bones.size(); b++) palette[b] = global[m_Bones[b].Node] * m_Bones[b].Offset;
    }

    glm::mat4 Model::GetGroundedMatrix(float targetSize) const {
        glm::vec3 size = m_Max - m_Min;
        float extent = std::max({ size.x, size.y, size.z });
        float s = extent > 0.0f ? targetSize / extent : 1.0f;
        glm::vec3 pivot((m_Min.x + m_Max.x) * 0.5f, m_Min.y, (m_Min.z + m_Max.z) * 0.5f);
        return glm::scale(glm::mat4(1.0f), glm::vec3(s)) * glm::translate(glm::mat4(1.0f), -pivot);
    }

    glm::mat4 Model::GetNormalizeMatrix(float targetSize) const {
        glm::vec3 size = m_Max - m_Min;
        float extent = std::max({ size.x, size.y, size.z });
        float s = extent > 0.0f ? targetSize / extent : 1.0f;
        glm::vec3 center = (m_Min + m_Max) * 0.5f;
        return glm::scale(glm::mat4(1.0f), glm::vec3(s)) * glm::translate(glm::mat4(1.0f), -center);
    }

} // namespace Engine