#include "Renderer/ParticleRenderer.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "Renderer/RenderStats.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_access.hpp>
#include <algorithm>

namespace Engine {

    ParticleRenderer::ParticleRenderer() {
        glCreateBuffers(1, &m_Buffer);
        glCreateVertexArrays(1, &m_VAO);       // the quad corners come from gl_VertexID: no attributes
    }

    ParticleRenderer::~ParticleRenderer() {
        glDeleteBuffers(1, &m_Buffer);
        glDeleteVertexArrays(1, &m_VAO);
    }

    void ParticleRenderer::Render(std::vector<ParticleDraw>& draws, const Camera& camera, Shader& shader) {
        draws.erase(std::remove_if(draws.begin(), draws.end(), [](const ParticleDraw& d) { return d.Instances.empty(); }), draws.end());
        if (draws.empty()) return;
        std::stable_sort(draws.begin(), draws.end(), [](const ParticleDraw& a, const ParticleDraw& b) { return a.Distance > b.Distance; });

        // The camera's right and up in world space: the rows of the view matrix
        const glm::mat4& view = camera.GetView();
        shader.Bind();
        shader.SetMat4("u_ViewProjection", camera.GetViewProjection());
        shader.SetVec3("u_CamRight", glm::vec3(glm::row(view, 0)));
        shader.SetVec3("u_CamUp", glm::vec3(glm::row(view, 1)));
        shader.SetInt("u_Texture", 0);

        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // leave the entity-ID target alone (not clickable)
        glBindVertexArray(m_VAO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_Buffer);

        for (const ParticleDraw& d : draws) {
            const size_t bytes = d.Instances.size() * sizeof(ParticleInstance);
            if (bytes > m_Capacity) m_Capacity = std::max(bytes, m_Capacity * 2);
            glNamedBufferData(m_Buffer, (GLsizeiptr)m_Capacity, nullptr, GL_STREAM_DRAW);   // orphan, then fill
            glNamedBufferSubData(m_Buffer, 0, (GLsizeiptr)bytes, d.Instances.data());

            if (d.Additive) glBlendFunci(0, GL_ONE, GL_ONE);
            else glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied "over"
            shader.SetInt("u_UseTexture", d.Texture ? 1 : 0);
            if (d.Texture) d.Texture->Bind(0);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)d.Instances.size());
            RenderStats::DrawCalls++;
            RenderStats::Particles += (uint32_t)d.Instances.size();
        }

        glBindVertexArray(0);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

} // namespace Engine
