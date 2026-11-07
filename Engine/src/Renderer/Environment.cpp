#include "Renderer/Environment.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image.h>      // implementation is compiled in Texture.cpp
#include <algorithm>

namespace Engine {

    static uint32_t MakeCube(uint32_t size, uint32_t levels) {
        uint32_t t = 0;
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &t);
        glTextureStorage2D(t, (GLsizei)levels, GL_RGBA16F, (GLsizei)size, (GLsizei)size);
        glTextureParameteri(t, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(t, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(t, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        return t;
    }

    Environment::Environment(const std::string& shaderDir) : m_ShaderDir(shaderDir) {
        glCreateVertexArrays(1, &m_VAO);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);   // filter across cube edges (needed by blurred mips)
        m_Sky = std::make_unique<Shader>(shaderDir + "sky.vert", shaderDir + "sky.frag");
    }

    Environment::~Environment() {
        DestroyMaps();
        glDeleteVertexArrays(1, &m_VAO);
    }

    void Environment::DestroyMaps() {
        if (m_Env)        glDeleteTextures(1, &m_Env);
        if (m_Irradiance) glDeleteTextures(1, &m_Irradiance);
        if (m_Prefilter)  glDeleteTextures(1, &m_Prefilter);
        m_Env = m_Irradiance = m_Prefilter = 0;
    }

    void Environment::ReloadSkyShader() { m_Sky->Reload(); }

    bool Environment::UseProcedural() {
        Bake(0, true);
        Name = "Procedural sky";
        return true;
    }

    bool Environment::LoadHDR(const std::string& path) {
        stbi_set_flip_vertically_on_load(false);      // panoramas are stored top row = straight up
        int w = 0, h = 0, channels = 0;
        float* pixels = stbi_loadf(path.c_str(), &w, &h, &channels, 3);
        if (!pixels) {
            ENGINE_ERROR("Failed to load environment '{}' (must be a Radiance .hdr file)", path);
            return false;
        }

        // Half floats overflow above 65504, and a real sun in a panorama can exceed that
        const size_t count = (size_t)w * h * 3;
        for (size_t i = 0; i < count; i++) pixels[i] = std::min(pixels[i], 60000.0f);

        uint32_t tex = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 1, GL_RGB16F, w, h);
        glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGB, GL_FLOAT, pixels);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(pixels);

        Bake(tex, false);
        glDeleteTextures(1, &tex);

        const size_t slash = path.find_last_of("/\\");
        Name = slash == std::string::npos ? path : path.substr(slash + 1);
        ENGINE_INFO("Environment '{}' baked ({}x{} source)", Name, w, h);
        return true;
    }

    void Environment::Bake(uint32_t equirect, bool procedural) {
        DestroyMaps();
        m_Env = MakeCube(kEnvSize, kEnvLevels);
        m_Irradiance = MakeCube(kIrradianceSize, 1);
        m_Prefilter = MakeCube(kPrefilterSize, kPrefilterLevels);

        const std::string vs = m_ShaderDir + "cubebake.vert";
        Shader toCube(vs, m_ShaderDir + "equirect.frag");
        Shader irradiance(vs, m_ShaderDir + "irradiance.frag");
        Shader prefilter(vs, m_ShaderDir + "prefilter.frag");

        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        uint32_t fbo = 0;
        glCreateFramebuffers(1, &fbo);
        glNamedFramebufferDrawBuffer(fbo, GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glBindVertexArray(m_VAO);

        // Render a fullscreen triangle into each of the 6 faces of one mip level
        auto pass = [&](Shader& sh, uint32_t tex, uint32_t level, uint32_t size) {
            glViewport(0, 0, (GLsizei)size, (GLsizei)size);
            for (uint32_t face = 0; face < 6; face++) {
                glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0, tex, (GLint)level, (GLint)face);
                sh.SetInt("u_Face", (int)face);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            };

        // 1) panorama (or procedural sky) -> environment cubemap
        toCube.Bind();
        toCube.SetInt("u_Equirect", 0);
        toCube.SetInt("u_Procedural", procedural ? 1 : 0);
        if (equirect) glBindTextureUnit(0, equirect);
        pass(toCube, m_Env, 0, kEnvSize);
        glGenerateTextureMipmap(m_Env);

        // 2) environment -> diffuse irradiance
        irradiance.Bind();
        irradiance.SetInt("u_Env", 0);
        glBindTextureUnit(0, m_Env);
        pass(irradiance, m_Irradiance, 0, kIrradianceSize);

        // 3) environment -> specular prefilter, one roughness per mip level
        prefilter.Bind();
        prefilter.SetInt("u_Env", 0);
        prefilter.SetFloat("u_Resolution", (float)kEnvSize);
        for (uint32_t mip = 0; mip < kPrefilterLevels; mip++) {
            prefilter.SetFloat("u_Roughness", (float)mip / (float)(kPrefilterLevels - 1));
            pass(prefilter, m_Prefilter, mip, kPrefilterSize >> mip);
        }

        glBindTextureUnit(0, 0);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    void Environment::BindLighting(uint32_t irradianceSlot, uint32_t prefilterSlot) const {
        glBindTextureUnit(irradianceSlot, m_Irradiance);
        glBindTextureUnit(prefilterSlot, m_Prefilter);
    }

    void Environment::DrawSkybox(const Camera& camera, float intensity) const {
        if (!Show || !m_Env) return;

        m_Sky->Bind();
        m_Sky->SetMat4("u_InvViewProj", glm::inverse(camera.GetViewProjection()));
        m_Sky->SetVec3("u_CamPos", camera.Position);
        m_Sky->SetInt("u_Env", 0);
        m_Sky->SetFloat("u_Lod", Blur);
        m_Sky->SetFloat("u_Intensity", intensity);
        m_Sky->SetFloat("u_EnvRot", glm::radians(RotationDegrees));

        glBindTextureUnit(0, m_Env);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);          // the sky never writes depth
        glDepthFunc(GL_LEQUAL);         // but passes where depth is still the cleared 1.0
        glBindVertexArray(m_VAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glBindTextureUnit(0, 0);
    }

} // namespace Engine