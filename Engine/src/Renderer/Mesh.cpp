#include "Renderer/RenderStats.h"
#include "Renderer/Mesh.h"
#include <glad/glad.h>

namespace Engine {

    Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        auto vb = std::make_shared<VertexBuffer>(
            reinterpret_cast<const float*>(vertices.data()),
            (uint32_t)(vertices.size() * sizeof(Vertex)));
        vb->SetLayout({
            { ShaderDataType::Float3 },   // position
            { ShaderDataType::Float3 },   // normal
            { ShaderDataType::Float2 },   // uv
            });

        m_VAO = std::make_unique<VertexArray>();
        m_VAO->AddVertexBuffer(vb);
        m_VAO->SetIndexBuffer(std::make_shared<IndexBuffer>(indices.data(), (uint32_t)indices.size()));

        if (!vertices.empty()) {
            m_Min = m_Max = vertices[0].Position;
            for (const auto& v : vertices) {
                m_Min = glm::min(m_Min, v.Position);
                m_Max = glm::max(m_Max, v.Position);
            }
        }
    }

    void Mesh::Draw() const {
        uint32_t count = m_VAO->GetIndexBuffer()->GetCount();
        RenderStats::DrawCalls++;
        RenderStats::Triangles += count / 3;

        m_VAO->Bind();
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
    }

    std::shared_ptr<Mesh> Mesh::CreateCube() {
        // Per face: normal (3 floats) then 4 corners (12 floats), counter-clockwise seen from outside
        const float faces[6][15] = {
            { 0, 0, 1,  -.5f,-.5f, .5f,   .5f,-.5f, .5f,   .5f, .5f, .5f,  -.5f, .5f, .5f },  // +Z
            { 0, 0,-1,   .5f,-.5f,-.5f,  -.5f,-.5f,-.5f,  -.5f, .5f,-.5f,   .5f, .5f,-.5f },  // -Z
            { -1, 0, 0,  -.5f,-.5f,-.5f,  -.5f,-.5f, .5f,  -.5f, .5f, .5f,  -.5f, .5f,-.5f },  // -X
            { 1, 0, 0,   .5f,-.5f, .5f,   .5f,-.5f,-.5f,   .5f, .5f,-.5f,   .5f, .5f, .5f },  // +X
            { 0, 1, 0,  -.5f, .5f, .5f,   .5f, .5f, .5f,   .5f, .5f,-.5f,  -.5f, .5f,-.5f },  // +Y
            { 0,-1, 0,  -.5f,-.5f,-.5f,   .5f,-.5f,-.5f,   .5f,-.5f, .5f,  -.5f,-.5f, .5f },  // -Y
        };
        const glm::vec2 uvs[4] = { glm::vec2(0,0), glm::vec2(1,0), glm::vec2(1,1), glm::vec2(0,1) };

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (uint32_t f = 0; f < 6; f++) {
            glm::vec3 n(faces[f][0], faces[f][1], faces[f][2]);
            for (int i = 0; i < 4; i++) {
                glm::vec3 p(faces[f][3 + i * 3], faces[f][4 + i * 3], faces[f][5 + i * 3]);
                vertices.push_back({ p, n, uvs[i] });
            }
            uint32_t b = f * 4;
            indices.insert(indices.end(), { b, b + 1, b + 2, b + 2, b + 3, b });
        }
        return std::make_shared<Mesh>(vertices, indices);
    }

    std::shared_ptr<Mesh> Mesh::CreatePlane(float size, float uvRepeat) {
        float s = size * 0.5f;
        glm::vec3 n(0, 1, 0);
        std::vector<Vertex> vertices = {
            { glm::vec3(-s, 0,  s), n, glm::vec2(0, 0) },
            { glm::vec3(s, 0,  s), n, glm::vec2(uvRepeat, 0) },
            { glm::vec3(s, 0, -s), n, glm::vec2(uvRepeat, uvRepeat) },
            { glm::vec3(-s, 0, -s), n, glm::vec2(0, uvRepeat) },
        };
        std::vector<uint32_t> indices = { 0, 1, 2, 2, 3, 0 };
        return std::make_shared<Mesh>(vertices, indices);
    }

} // namespace Engine