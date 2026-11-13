#include "Renderer/RenderStats.h"
#include "Renderer/Mesh.h"
#include <glad/glad.h>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>
#include <cassert>

namespace Engine {

    uint32_t PackVertexColor(float r, float g, float b, float a) {
        auto enc = [](float c) {   // the shader decodes with pow(c, 2.2), like Material colours
            return (uint32_t)std::lround(std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
        };
        const uint32_t alpha = (uint32_t)std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        return enc(r) | (enc(g) << 8) | (enc(b) << 16) | (alpha << 24);
    }

    uint32_t PackVertexWind(float r, float g, float b, float a) {
        auto q = [](float c) { return (uint32_t)std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f); };
        return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
    }

    Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        auto vb = std::make_shared<VertexBuffer>(
            reinterpret_cast<const float*>(vertices.data()),
            (uint32_t)(vertices.size() * sizeof(Vertex)));
        vb->SetLayout({
            { ShaderDataType::Float3 },   // position
            { ShaderDataType::Float3 },   // normal
            { ShaderDataType::Float2 },   // uv
            { ShaderDataType::UByte4Norm, 5 },   // vertex colour (3 and 4 are the bone attributes)
            { ShaderDataType::UByte4Norm, 6 },   // wind weights
            });

        assert(vb->GetLayout().GetStride() == sizeof(Vertex) && "the layout must match Vertex");
        m_VAO = std::make_unique<VertexArray>();
        m_VAO->AddVertexBuffer(vb);
        m_VAO->SetIndexBuffer(std::make_shared<IndexBuffer>(indices.data(), (uint32_t)indices.size()));

        m_Vertices = vertices;
        m_Positions.reserve(vertices.size());
        for (const auto& v : vertices) m_Positions.push_back(v.Position);
        m_Indices = indices;

        if (!vertices.empty()) {
            m_Min = m_Max = vertices[0].Position;
            for (const auto& v : vertices) {
                m_Min = glm::min(m_Min, v.Position);
                m_Max = glm::max(m_Max, v.Position);
            }
        }
    }

    Mesh::Mesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t>& indices,
        const std::vector<glm::vec3>& restPositions) {
        auto vb = std::make_shared<VertexBuffer>(
            reinterpret_cast<const float*>(vertices.data()),
            (uint32_t)(vertices.size() * sizeof(SkinnedVertex)));
        vb->SetLayout({
            { ShaderDataType::Float3 },   // position
            { ShaderDataType::Float3 },   // normal
            { ShaderDataType::Float2 },   // uv
            { ShaderDataType::Int4 },     // bone indices
            { ShaderDataType::Float4 },   // bone weights
            { ShaderDataType::UByte4Norm, 5 },   // vertex colour
            { ShaderDataType::UByte4Norm, 6 },   // wind weights
            });

        assert(vb->GetLayout().GetStride() == sizeof(SkinnedVertex) && "the layout must match SkinnedVertex");
        m_VAO = std::make_unique<VertexArray>();
        m_VAO->AddVertexBuffer(vb);
        m_VAO->SetIndexBuffer(std::make_shared<IndexBuffer>(indices.data(), (uint32_t)indices.size()));

        m_Positions = restPositions;
        m_Indices = indices;
        if (!m_Positions.empty()) {
            m_Min = m_Max = m_Positions[0];
            for (const auto& p : m_Positions) {
                m_Min = glm::min(m_Min, p);
                m_Max = glm::max(m_Max, p);
            }
        }
    }

    void Mesh::GetCollisionGeometry(std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices) const {
        const uint32_t base = (uint32_t)positions.size();
        positions.insert(positions.end(), m_Positions.begin(), m_Positions.end());
        for (uint32_t i : m_Indices) indices.push_back(base + i);
    }

    bool Mesh::GetPartVertices(int32_t, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) const {
        if (m_Vertices.empty()) return false;
        const uint32_t base = (uint32_t)vertices.size();
        vertices.insert(vertices.end(), m_Vertices.begin(), m_Vertices.end());
        for (uint32_t i : m_Indices) indices.push_back(base + i);
        return true;
    }

    void Mesh::Draw() const {
        uint32_t count = m_VAO->GetIndexBuffer()->GetCount();
        RenderStats::DrawCalls++;
        RenderStats::Triangles += count / 3;

        m_VAO->Bind();
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
    }

    void Mesh::DrawInstanced(uint32_t instances) const {
        if (instances == 0) return;
        uint32_t count = m_VAO->GetIndexBuffer()->GetCount();
        RenderStats::DrawCalls++;
        RenderStats::Triangles += count / 3 * instances;
        RenderStats::Instances += instances;
        m_VAO->Bind();
        glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT, nullptr, (GLsizei)instances);
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

    std::shared_ptr<Mesh> Mesh::CreateSphere(uint32_t segments) {
        segments = std::max(segments, 8u);
        const uint32_t rings = segments / 2;

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        for (uint32_t y = 0; y <= rings; y++) {
            float v = (float)y / rings;
            float phi = v * glm::pi<float>();                  // 0 at the top pole, pi at the bottom
            for (uint32_t x = 0; x <= segments; x++) {
                float u = (float)x / segments;
                float theta = u * glm::two_pi<float>();
                glm::vec3 n(std::cos(theta) * std::sin(phi), std::cos(phi), std::sin(theta) * std::sin(phi));
                vertices.push_back({ n * 0.5f, n, glm::vec2(u, v) });
            }
        }
        for (uint32_t y = 0; y < rings; y++) {
            for (uint32_t x = 0; x < segments; x++) {
                uint32_t a = y * (segments + 1) + x;
                uint32_t b = a + segments + 1;
                indices.insert(indices.end(), { a, a + 1, b,   a + 1, b + 1, b });   // counter-clockwise from outside
            }
        }
        return std::make_shared<Mesh>(vertices, indices);
    }

    namespace {
        // One ring of a surface of revolution: radius and height, and the normal in the (radius, up) plane
        struct ProfilePoint { float R, Y, NR, NY; };

        // Spin a profile (top to bottom) around +Y, like CreateSphere (same angle and index order, so the same
        // counter-clockwise winding). Repeat a point with a different normal for a hard edge.
        std::shared_ptr<Mesh> Lathe(const std::vector<ProfilePoint>& profile, uint32_t segments) {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const uint32_t rows = (uint32_t)profile.size();
            for (uint32_t r = 0; r < rows; r++) {
                const ProfilePoint& p = profile[r];
                const float v = rows > 1 ? (float)r / (rows - 1) : 0.0f;
                for (uint32_t x = 0; x <= segments; x++) {
                    const float u = (float)x / segments;
                    const float theta = u * glm::two_pi<float>();
                    const float c = std::cos(theta), s = std::sin(theta);
                    vertices.push_back({ glm::vec3(c * p.R, p.Y, s * p.R), glm::normalize(glm::vec3(c * p.NR, p.NY, s * p.NR)), glm::vec2(u, v) });
                }
            }
            for (uint32_t r = 0; r + 1 < rows; r++)
                for (uint32_t x = 0; x < segments; x++) {
                    const uint32_t a = r * (segments + 1) + x, b = a + segments + 1;
                    indices.insert(indices.end(), { a, a + 1, b,   a + 1, b + 1, b });
                }
            return std::make_shared<Mesh>(vertices, indices);
        }
    }

    // 1 wide, 2 tall (two half-spheres of radius 0.5 around a 1 m middle), centred on the origin
    std::shared_ptr<Mesh> Mesh::CreateCapsule(uint32_t segments) {
        segments = std::max(segments, 8u);
        const uint32_t half = segments / 4;
        std::vector<ProfilePoint> profile;
        for (uint32_t i = 0; i <= half; i++) {                       // top half-sphere, pole to equator
            const float phi = (float)i / half * glm::half_pi<float>();
            profile.push_back({ 0.5f * std::sin(phi), 0.5f + 0.5f * std::cos(phi), std::sin(phi), std::cos(phi) });
        }
        for (uint32_t i = 0; i <= half; i++) {                       // bottom half-sphere, equator to pole
            const float phi = glm::half_pi<float>() + (float)i / half * glm::half_pi<float>();
            profile.push_back({ 0.5f * std::sin(phi), -0.5f + 0.5f * std::cos(phi), std::sin(phi), std::cos(phi) });
        }
        return Lathe(profile, segments);
    }

    // 1 wide, 1 tall, centred on the origin, with flat caps
    std::shared_ptr<Mesh> Mesh::CreateCylinder(uint32_t segments) {
        return Lathe({ { 0.0f, 0.5f, 0, 1 }, { 0.5f, 0.5f, 0, 1 },          // top cap
                       { 0.5f, 0.5f, 1, 0 }, { 0.5f, -0.5f, 1, 0 },         // side
                       { 0.5f, -0.5f, 0, -1 }, { 0.0f, -0.5f, 0, -1 } },    // bottom cap
                     std::max(segments, 8u));
    }

    // 1 wide at the base (y = -0.5), tip at y = +0.5, with a flat base
    std::shared_ptr<Mesh> Mesh::CreateCone(uint32_t segments) {
        return Lathe({ { 0.0f, 0.5f, 1.0f, 0.5f }, { 0.5f, -0.5f, 1.0f, 0.5f },   // side (normal tilted up by the slope)
                       { 0.5f, -0.5f, 0, -1 }, { 0.0f, -0.5f, 0, -1 } },        // base
                     std::max(segments, 8u));
    }

} // namespace Engine