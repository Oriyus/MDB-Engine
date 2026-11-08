#include "Renderer/DecalRenderer.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "Renderer/RenderStats.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <numeric>

namespace Engine {

    namespace {
        // Matches DecalData in decal.vert / decal.frag (std430)
        struct GpuDecal {
            glm::mat4 Model;
            glm::mat4 InvModel;
            glm::vec4 Color;
            glm::vec4 Params;
            glm::vec4 Extra;
        };
        static_assert(sizeof(GpuDecal) == 176, "GpuDecal must match the std430 layout");
    }

    DecalRenderer::DecalRenderer() {
        glCreateBuffers(1, &m_Buffer);
        glCreateVertexArrays(1, &m_VAO);       // box corners come from gl_VertexID: no attributes
    }

    DecalRenderer::~DecalRenderer() {
        glDeleteBuffers(1, &m_Buffer);
        glDeleteVertexArrays(1, &m_VAO);
    }

    void DecalRenderer::Render(const std::vector<Decal>& decals, const Camera& camera, uint32_t depthCopy,
        uint32_t width, uint32_t height, Shader& shader) {
        if (decals.empty()) return;

        // Untextured decals first (one draw), then one draw per texture
        std::vector<size_t> order(decals.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return decals[a].Texture.get() < decals[b].Texture.get();
        });

        std::vector<GpuDecal> gpu;
        gpu.reserve(decals.size());
        for (size_t i : order) {
            const Decal& d = decals[i];
            const glm::vec3 size = glm::max(d.Size, glm::vec3(0.001f));
            glm::mat4 m = glm::translate(glm::mat4(1.0f), d.Position);
            m = glm::rotate(m, d.Yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::scale(m, size);
            const int shape = d.Kind == Decal::Shape::Square ? 1 : (d.Kind == Decal::Shape::Texture && d.Texture) ? 2 : 0;
            gpu.push_back({ m, glm::inverse(m), glm::vec4(d.Color, d.Alpha),
                            glm::vec4((float)shape, d.Thickness, d.Fill, d.Softness), glm::vec4(d.Emission, 0, 0, 0) });
        }
        const size_t bytes = gpu.size() * sizeof(GpuDecal);
        if (bytes > m_Capacity) m_Capacity = std::max(bytes, m_Capacity * 2);
        glNamedBufferData(m_Buffer, (GLsizeiptr)m_Capacity, nullptr, GL_STREAM_DRAW);   // orphan, then fill
        glNamedBufferSubData(m_Buffer, 0, (GLsizeiptr)bytes, gpu.data());

        shader.Bind();
        shader.SetMat4("u_ViewProjection", camera.GetViewProjection());
        shader.SetMat4("u_InvViewProjection", glm::inverse(camera.GetViewProjection()));
        shader.SetVec2("u_ViewSize", glm::vec2((float)width, (float)height));
        shader.SetInt("u_Depth", 0);
        shader.SetInt("u_Texture", 1);                 // its own unit, even when unused
        glBindTextureUnit(0, depthCopy);

        glEnable(GL_BLEND);
        glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // not clickable: leave the entity IDs alone
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);                      // the shader compares against the depth itself
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);                          // back faces: works with the camera inside a box
        glBindVertexArray(m_VAO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Buffer);

        size_t start = 0;
        while (start < order.size()) {
            const Texture2D* tex = decals[order[start]].Texture.get();
            size_t end = start + 1;
            while (end < order.size() && decals[order[end]].Texture.get() == tex) end++;
            shader.SetInt("u_UseTexture", tex ? 1 : 0);
            if (tex) tex->Bind(1);
            shader.SetInt("u_Base", (int)start);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 36, (GLsizei)(end - start));
            RenderStats::DrawCalls++;
            start = end;
        }
        RenderStats::Decals += (uint32_t)decals.size();

        glBindVertexArray(0);
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
    }

} // namespace Engine
