#include "Renderer/SceneRenderer.h"
#include "Renderer/Shader.h"
#include "Renderer/DrawList.h"
#include "Renderer/ParticleRenderer.h"
#include "Renderer/DecalRenderer.h"
#include "Renderer/FoliageRenderer.h"
#include "Renderer/VolumetricFog.h"
#include "Renderer/Camera.h"
#include "Renderer/Framebuffer.h"
#include "Renderer/Environment.h"
#include "Renderer/SSAO.h"
#include "Renderer/Bloom.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"
#include "Core/Profiler.h"

#include <glad/glad.h>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace Engine {

    struct SceneRenderer::Impl {
        std::string AssetDir;                    // the project's Assets (Environments)
        std::string EngineDir;                   // the engine's files (Shaders)
        std::unique_ptr<Shader> Lit, PointDepth, SunDepth, Post, SSAOShader, SSAOBlur, BloomDown, BloomUp, FXAA, ParticleShader, DecalShader;
        std::unique_ptr<DecalRenderer> Decals;
        std::unique_ptr<Shader> FoliageShader;           // foliage.vert + lit.frag: grass lit like everything else
        std::unique_ptr<FoliageRenderer> Foliage;
        std::unique_ptr<Shader> VolFogMarch, VolFogComposite;   // volfog.frag (half resolution), volfog_composite.frag
        std::unique_ptr<VolumetricFog> FogPass;
        std::chrono::steady_clock::time_point WindClock = std::chrono::steady_clock::now();   // advances Scene::WindTime
        uint32_t DepthCopy = 0, DepthCopyW = 0, DepthCopyH = 0;   // the scene depth, for decals to sample
        uint32_t SelDepth = 0, SelDepthW = 0, SelDepthH = 0;      // the depth of the solid scene, for the selection outline

        // Fog of war: the map as an RG8 texture (re-uploaded when its revision changes) and a fullscreen pass
        std::unique_ptr<Shader> FogShader;
        uint32_t FogTex = 0, FogVAO = 0, FogRevision = 0;
        int FogW = 0, FogD = 0;

        // Editor views: wireframe (lit.vert + wire.frag), the selection mask (lit.vert + select_mask.frag, an RG8
        // target) and its outline (a fullscreen pass over the final image)
        std::unique_ptr<Shader> Wire, SelectMask, Outline;
        uint32_t MaskTex = 0, MaskFBO = 0, MaskW = 0, MaskH = 0;
        std::vector<uint32_t> HighlightSorted;

        ~Impl() {
            if (DepthCopy) glDeleteTextures(1, &DepthCopy);
            if (SelDepth) glDeleteTextures(1, &SelDepth);
            if (FogTex) glDeleteTextures(1, &FogTex);
            if (FogVAO) glDeleteVertexArrays(1, &FogVAO);
            if (MaskFBO) glDeleteFramebuffers(1, &MaskFBO);
            if (MaskTex) glDeleteTextures(1, &MaskTex);
        }

        // Wireframe: fill once for depth and entity IDs (colour off), then the edges as lines on top
        void DrawWireframe(Scene& scene, const Camera& camera, bool xray) {
            Shader& s = *Wire;
            s.Bind();
            s.SetVec3("u_WireColor", glm::vec3(0.55f, 0.58f, 0.62f));
            s.SetInt("u_WirePass", 0);
            glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            scene.Render(camera, s, *Draws, nullptr, nullptr, Scene::RenderPass::Opaque);
            scene.Render(camera, s, *Draws, nullptr, nullptr, Scene::RenderPass::Transparent);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            s.SetInt("u_WirePass", 1);
            glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // the IDs come from the fill
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            if (xray) { glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); }
            else {
                glDepthFunc(GL_LEQUAL);
                glEnable(GL_POLYGON_OFFSET_LINE);                        // lines win over their own filled faces
                glPolygonOffset(-1.0f, -1.0f);
            }
            glDepthMask(GL_FALSE);
            scene.Render(camera, s, *Draws, nullptr, nullptr, Scene::RenderPass::Opaque);
            scene.Render(camera, s, *Draws, nullptr, nullptr, Scene::RenderPass::Transparent);
            glDepthMask(GL_TRUE);
            glDisable(GL_POLYGON_OFFSET_LINE);
            glDepthFunc(GL_LESS);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }

        // The selected parts into the mask: R where they are in front of the scene depth, G where hidden
        void DrawSelectionMask(Scene& scene, const Camera& camera, uint32_t depthTex, uint32_t width, uint32_t height) {
            if (!MaskTex || MaskW != width || MaskH != height) {
                if (MaskFBO) glDeleteFramebuffers(1, &MaskFBO);
                if (MaskTex) glDeleteTextures(1, &MaskTex);
                glCreateTextures(GL_TEXTURE_2D, 1, &MaskTex);
                glTextureStorage2D(MaskTex, 1, GL_RG8, (GLsizei)width, (GLsizei)height);
                glTextureParameteri(MaskTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(MaskTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(MaskTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(MaskTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glCreateFramebuffers(1, &MaskFBO);
                glNamedFramebufferTexture(MaskFBO, GL_COLOR_ATTACHMENT0, MaskTex, 0);
                MaskW = width;
                MaskH = height;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, MaskFBO);
            glViewport(0, 0, (GLsizei)width, (GLsizei)height);
            const float zero[4] = { 0, 0, 0, 0 };
            glClearNamedFramebufferfv(MaskFBO, GL_COLOR, 0, zero);
            Shader& s = *SelectMask;
            s.Bind();
            s.SetInt("u_SceneDepth", 8);
            glBindTextureUnit(8, depthTex);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendEquation(GL_MAX);
            glBlendFunc(GL_ONE, GL_ONE);
            scene.Render(camera, s, *Draws, nullptr, nullptr, Scene::RenderPass::Selected);
            glBlendEquation(GL_FUNC_ADD);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            glBindTextureUnit(8, 0);
        }

        void DrawOutline(uint32_t targetFBO, uint32_t width, uint32_t height, const glm::vec3& color, float lineWidth) {
            if (!FogVAO) glCreateVertexArrays(1, &FogVAO);
            glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
            glViewport(0, 0, (GLsizei)width, (GLsizei)height);
            Shader& s = *Outline;
            s.Bind();
            s.SetInt("u_Mask", 0);
            s.SetVec3("u_Color", color);
            s.SetFloat("u_Width", std::clamp(lineWidth, 0.5f, 8.0f));
            glBindTextureUnit(0, MaskTex);
            s.SetInt("u_IDs", 1);
            glBindTextureUnit(1, HDR->GetEntityIDAttachment());   // lines between selected objects that touch
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindVertexArray(FogVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glDisable(GL_BLEND);
            glEnable(GL_CULL_FACE);
            glEnable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        void DrawFog(const FogOfWarState& fog, const Camera& camera, uint32_t depthTex) {
            if (!FogTex || FogW != fog.Width || FogD != fog.Depth) {
                if (FogTex) glDeleteTextures(1, &FogTex);
                glCreateTextures(GL_TEXTURE_2D, 1, &FogTex);
                glTextureStorage2D(FogTex, 1, GL_RG8, fog.Width, fog.Depth);
                glTextureParameteri(FogTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(FogTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);   // soft edges between cells
                glTextureParameteri(FogTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                glTextureParameteri(FogTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                const float border[4] = { 0, 0, 0, 0 };                           // outside the map: never seen
                glTextureParameterfv(FogTex, GL_TEXTURE_BORDER_COLOR, border);
                FogW = fog.Width;
                FogD = fog.Depth;
                FogRevision = fog.Revision - 1;
            }
            if (FogRevision != fog.Revision && fog.Texels.size() >= size_t(FogW) * FogD * 2) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTextureSubImage2D(FogTex, 0, 0, 0, FogW, FogD, GL_RG, GL_UNSIGNED_BYTE, fog.Texels.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                FogRevision = fog.Revision;
            }
            if (!FogVAO) glCreateVertexArrays(1, &FogVAO);
            auto lin = [](const glm::vec3& c) { return glm::pow(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f)); };
            Shader& s = *FogShader;
            s.Bind();
            s.SetInt("u_Depth", 0);
            s.SetInt("u_Fog", 1);
            s.SetMat4("u_InvViewProjection", glm::inverse(camera.GetViewProjection()));
            s.SetVec2("u_FogOrigin", fog.Origin);
            s.SetVec2("u_FogSize", glm::vec2(fog.Width, fog.Depth) * fog.CellSize);
            s.SetVec3("u_HiddenColor", lin(fog.HiddenColor));
            s.SetVec3("u_ExploredColor", lin(fog.ExploredColor));
            s.SetFloat("u_ExploredAlpha", fog.ExploredAlpha);
            glBindTextureUnit(0, depthTex);
            glBindTextureUnit(1, FogTex);
            glEnable(GL_BLEND);
            glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // keep the entity IDs (clicks still pick)
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glBindVertexArray(FogVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Copy the HDR target's depth (decals read it while it is still attached for drawing)
        uint32_t CopyDepth(uint32_t width, uint32_t height) { return CopyDepthInto(DepthCopy, DepthCopyW, DepthCopyH, width, height); }
        uint32_t CopyDepthInto(uint32_t& tex, uint32_t& texW, uint32_t& texH, uint32_t width, uint32_t height) {
            if (!tex || texW != width || texH != height) {
                if (tex) glDeleteTextures(1, &tex);
                glCreateTextures(GL_TEXTURE_2D, 1, &tex);
                glTextureStorage2D(tex, 1, GL_DEPTH24_STENCIL8, (GLsizei)width, (GLsizei)height);
                glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                texW = width;
                texH = height;
            }
            glCopyImageSubData(HDR->GetDepthAttachment(), GL_TEXTURE_2D, 0, 0, 0, 0,
                tex, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height, 1);
            return tex;
        }
        std::unique_ptr<ParticleRenderer> Particles;
        std::vector<ParticleDraw> ParticleDraws;           // reused each frame
        std::unique_ptr<DrawList> Draws = std::make_unique<DrawList>();   // this frame's batches and instances
        std::unique_ptr<Environment> Env;
        std::unique_ptr<PointShadowArray> PointShadows;
        std::unique_ptr<DirectionalShadowMap> SunShadow;
        ShadowContext Shadows;
        std::unique_ptr<Framebuffer> HDR;
        std::unique_ptr<SSAO> AO;
        std::unique_ptr<Bloom> Glow;
        std::unique_ptr<PostProcess> Final;

        bool EnvPending = false;
        std::string EnvRequested;                // "" = procedural

        std::string ShaderPath(const char* name) const { return EngineDir + "Shaders/" + name; }

        void BakePendingEnvironment() {
            if (!EnvPending) return;
            EnvPending = false;
            if (EnvRequested.empty()) Env->UseProcedural();
            else if (!Env->LoadHDR(AssetDir + "Environments/" + EnvRequested)) {
                ENGINE_WARN("Environment '{}' could not be loaded, using the procedural sky", EnvRequested);
                Env->UseProcedural();
            }
        }
    };

    SceneRenderer::SceneRenderer(const std::string& engineDir, const std::string& assetDir, uint32_t width, uint32_t height)
        : m_Impl(std::make_unique<Impl>()) {
        Impl& I = *m_Impl;
        I.AssetDir = assetDir;
        I.EngineDir = engineDir;
        I.Lit = std::make_unique<Shader>(I.ShaderPath("lit.vert"), I.ShaderPath("lit.frag"));
        I.PointDepth = std::make_unique<Shader>(I.ShaderPath("shadow.vert"), I.ShaderPath("shadow.frag"));
        I.SunDepth = std::make_unique<Shader>(I.ShaderPath("shadow.vert"), I.ShaderPath("sun_shadow.frag"));
        I.ParticleShader = std::make_unique<Shader>(I.ShaderPath("particle.vert"), I.ShaderPath("particle.frag"));
        I.Particles = std::make_unique<ParticleRenderer>();
        I.DecalShader = std::make_unique<Shader>(I.ShaderPath("decal.vert"), I.ShaderPath("decal.frag"));
        I.Decals = std::make_unique<DecalRenderer>();
        I.FoliageShader = std::make_unique<Shader>(I.ShaderPath("foliage.vert"), I.ShaderPath("lit.frag"));
        I.Foliage = std::make_unique<FoliageRenderer>();
        I.VolFogMarch = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("volfog.frag"));
        I.VolFogComposite = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("volfog_composite.frag"));
        I.FogPass = std::make_unique<VolumetricFog>();
        I.FogShader = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("fog.frag"));
        I.Post = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("tonemap.frag"));
        I.SSAOShader = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("ssao.frag"));
        I.SSAOBlur = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("ssao_blur.frag"));
        I.BloomDown = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("bloom_down.frag"));
        I.BloomUp = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("bloom_up.frag"));
        I.FXAA = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("fxaa.frag"));
        I.Wire = std::make_unique<Shader>(I.ShaderPath("lit.vert"), I.ShaderPath("wire.frag"));
        I.SelectMask = std::make_unique<Shader>(I.ShaderPath("lit.vert"), I.ShaderPath("select_mask.frag"));
        I.Outline = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("outline.frag"));
        I.Wire->QuietMissing = true;          // given the lit.frag lighting uniforms (Scene::Render)
        I.SelectMask->QuietMissing = true;

        I.Env = std::make_unique<Environment>(engineDir + "Shaders/");
        I.Env->UseProcedural();

        I.PointShadows = std::make_unique<PointShadowArray>(1024);
        I.SunShadow = std::make_unique<DirectionalShadowMap>(2048);
        I.Shadows.Points = I.PointShadows.get();
        I.Shadows.Sun = I.SunShadow.get();
        I.Shadows.PointDepth = I.PointDepth.get();
        I.Shadows.SunDepth = I.SunDepth.get();

        I.HDR = std::make_unique<Framebuffer>(width, height);
        I.AO = std::make_unique<SSAO>(width, height);
        I.Glow = std::make_unique<Bloom>(width, height);
        I.Final = std::make_unique<PostProcess>(width, height);
    }

    SceneRenderer::~SceneRenderer() = default;

    void SceneRenderer::Render(Scene& scene, const Camera& camera, uint32_t width, uint32_t height) {
        Impl& I = *m_Impl;
        I.BakePendingEnvironment();
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        I.HDR->Resize(width, height);

        // The wind keeps blowing in Edit mode and while paused (trees sway; see Scene::Wind)
        const auto now = std::chrono::steady_clock::now();
        scene.WindTime += std::clamp(std::chrono::duration<float>(now - I.WindClock).count(), 0.0f, 0.1f);
        I.WindClock = now;

        const bool rendered = Mode == ViewMode::Rendered;
        const bool wireframe = Mode == ViewMode::Wireframe;
        ShadowContext* shadows = rendered ? &I.Shadows : nullptr;     // the editor's simple views have no shadows
        const Environment* env = rendered ? I.Env.get() : nullptr;

        // The selection to outline (sorted for BuildDrawList's lookups)
        I.HighlightSorted = Highlight;
        std::sort(I.HighlightSorted.begin(), I.HighlightSorted.end());
        I.Draws->Highlight = &I.HighlightSorted;

        // 1) Depth passes (all point lights + sun)
        { PROFILE_SCOPE("Build draw list"); scene.BuildDrawList(*I.Draws, camera, shadows); }
        if (shadows) { PROFILE_SCOPE("Shadow passes"); scene.RenderShadows(I.Shadows, *I.Draws); }

        // 2) Scene into the HDR framebuffer, then the sky behind it
        I.HDR->Bind();
        // Rendered: near black under the sky. The editor views: a flat grey (linear; about 0.2 on screen)
        if (rendered) I.HDR->Clear(0.005f, 0.005f, 0.01f, 1.0f);
        else if (wireframe) I.HDR->Clear(0.018f, 0.019f, 0.022f, 1.0f);
        else I.HDR->Clear(0.045f, 0.047f, 0.052f, 1.0f);
        const int viewMode = Mode == ViewMode::Solid ? 1 : Mode == ViewMode::Textured ? 2 : 0;
        I.Lit->SetInt("u_ViewMode", viewMode);
        I.FoliageShader->SetInt("u_ViewMode", viewMode);

        uint32_t depthCopy = 0;
        auto depth = [&]() { if (!depthCopy) depthCopy = I.CopyDepth(I.HDR->GetWidth(), I.HDR->GetHeight()); return depthCopy; };
        uint32_t selDepth = 0;                       // depth for the selection outline (copied below when needed)
        if (wireframe) {
            PROFILE_SCOPE("Wireframe pass");
            I.DrawWireframe(scene, camera, XRay);
            if (!I.Draws->Selected.empty()) selDepth = I.CopyDepthInto(I.SelDepth, I.SelDepthW, I.SelDepthH, I.HDR->GetWidth(), I.HDR->GetHeight());
        }
        else {
            { PROFILE_SCOPE("Opaque pass"); scene.Render(camera, *I.Lit, *I.Draws, shadows, env); }
            // The selection outline tests against the depth of solid objects only: grass, decals and glass do not hide it
            if (!I.Draws->Selected.empty()) selDepth = I.CopyDepthInto(I.SelDepth, I.SelDepthW, I.SelDepthH, I.HDR->GetWidth(), I.HDR->GetHeight());
            { PROFILE_SCOPE("Foliage"); I.Foliage->Render(scene, camera, *I.FoliageShader, *I.Draws, shadows, env); }   // grass: opaque, before the sky
            if (rendered) I.Env->DrawSkybox(camera, scene.AmbientIntensity);
            // Decals and fog both read the depth; it is copied once (after the opaque scene, the only depth writer)
            if (!scene.Decals.empty() && Mode != ViewMode::Solid)      // projected onto the opaque scene, under glass
                I.Decals->Render(scene.Decals, camera, depth(), I.HDR->GetWidth(), I.HDR->GetHeight(), *I.DecalShader);
            { PROFILE_SCOPE("Transparent pass"); scene.Render(camera, *I.Lit, *I.Draws, shadows, env, Scene::RenderPass::Transparent); }   // glass etc. over the sky
            if (VolFog.Enabled && rendered)                          // fog over the scene and the sky, under particles
                I.FogPass->Render(scene, camera, *I.VolFogMarch, *I.VolFogComposite, *I.Draws, &I.Shadows, I.Env.get(), depth(),
                                 I.HDR->GetWidth(), I.HDR->GetHeight(), VolFog);
            scene.GatherParticles(camera, I.ParticleDraws);             // particles last: over glass and sky
            I.Particles->Render(I.ParticleDraws, camera, *I.ParticleShader);
            if (scene.Fog.Enabled && ShowFog && scene.Fog.Width > 0 && rendered)   // fog of war over everything but the sky
                I.DrawFog(scene.Fog, camera, depth());
        }
        Framebuffer::Unbind();
        const bool outline = !I.Draws->Selected.empty();
        if (outline) { PROFILE_SCOPE("Selection mask"); I.DrawSelectionMask(scene, camera, selDepth, I.HDR->GetWidth(), I.HDR->GetHeight()); }
        PROFILE_SCOPE("Post (SSAO, bloom, tone map, FXAA)");
        // The editor views show the picture as it is: no bloom, exposure, tone curve or grading
        PostSettings post = Post;
        if (!rendered) {
            PostSettings plain;
            plain.ToneMap = 0;
            plain.AOEnabled = Post.AOEnabled && !wireframe;          // AO helps read shapes in solid views
            plain.AOStrength = Post.AOStrength;
            plain.AODebug = false;
            plain.BloomEnabled = false;
            plain.FXAA = Post.FXAA;
            post = plain;
        }

        // 3) Screen-space effects
        uint32_t aoTex = 0, bloomTex = 0;
        if (post.AOEnabled) {
            I.AO->Resize(width, height);
            I.AO->Compute(I.HDR->GetDepthAttachment(), camera.GetProjection(), *I.SSAOShader, *I.SSAOBlur);
            aoTex = I.AO->GetOutput();
        }
        if (post.BloomEnabled) {
            I.Glow->Resize(width, height);
            I.Glow->Compute(I.HDR->GetColorAttachment(), *I.BloomDown, *I.BloomUp);
            bloomTex = I.Glow->GetOutput();
        }

        // 4) AO + bloom + exposure + tone map + gamma into an 8-bit texture, then FXAA
        I.Final->Resize(width, height);
        I.Final->Apply(I.HDR->GetColorAttachment(), aoTex, bloomTex, *I.Post, post);
        I.Final->ApplyFXAA(*I.FXAA, post.FXAA);

        // 5) The selection outline over the finished picture (exact colour: after the tone map and grading)
        if (outline) I.DrawOutline(I.Final->GetOutputFramebuffer(), width, height, OutlineColor, OutlineWidth);
    }

    uint32_t SceneRenderer::GetOutput() const { return m_Impl->Final->GetOutput(); }
    uint32_t SceneRenderer::GetWidth() const { return m_Impl->HDR->GetWidth(); }
    uint32_t SceneRenderer::GetHeight() const { return m_Impl->HDR->GetHeight(); }
    Framebuffer& SceneRenderer::GetFramebuffer() { return *m_Impl->HDR; }
    ShadowSettings& SceneRenderer::Shadows() { return m_Impl->Shadows.Settings; }
    SSAO& SceneRenderer::GetSSAO() { return *m_Impl->AO; }
    Bloom& SceneRenderer::GetBloom() { return *m_Impl->Glow; }
    Environment& SceneRenderer::GetEnvironment() { return *m_Impl->Env; }

    void SceneRenderer::ReloadShaders() {
        Impl& I = *m_Impl;
        for (auto* s : { I.Lit.get(), I.PointDepth.get(), I.SunDepth.get(), I.Post.get(), I.SSAOShader.get(),
                         I.SSAOBlur.get(), I.BloomDown.get(), I.BloomUp.get(), I.FXAA.get(), I.ParticleShader.get(), I.DecalShader.get(), I.FogShader.get(), I.FoliageShader.get(), I.VolFogMarch.get(), I.VolFogComposite.get(),
                         I.Wire.get(), I.SelectMask.get(), I.Outline.get() })
            s->Reload();
        I.Env->ReloadSkyShader();
    }

    void SceneRenderer::RequestEnvironment(const std::string& hdrFile) {
        m_Impl->EnvRequested = hdrFile;
        m_Impl->EnvPending = true;
    }

    std::string SceneRenderer::CurrentEnvironment() const {
        if (m_Impl->EnvPending) return m_Impl->EnvRequested;
        return m_Impl->Env->Name == "Procedural sky" ? "" : m_Impl->Env->Name;
    }

    std::vector<std::string> SceneRenderer::ListEnvironments() const {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(m_Impl->AssetDir) / "Environments", ec))
            if (f.path().extension() == ".hdr") files.push_back(f.path().filename().string());
        std::sort(files.begin(), files.end());
        return files;
    }

    void SceneRenderer::ApplySettings(const SceneSettings& s) {
        Impl& I = *m_Impl;
        if (s.EnvironmentMap != CurrentEnvironment()) {
            const auto files = ListEnvironments();
            if (s.EnvironmentMap.empty() || std::find(files.begin(), files.end(), s.EnvironmentMap) != files.end())
                RequestEnvironment(s.EnvironmentMap);
            else
                ENGINE_WARN("Scene uses environment '{}', which is not in Assets/Environments", s.EnvironmentMap);
        }
        I.Env->Show = s.ShowSky;
        I.Env->Blur = s.SkyBlur;
        I.Env->RotationDegrees = s.SkyRotation;
        Post = s.Post;
        I.Shadows.Settings = s.Shadows;
        I.AO->Radius = s.AORadius;
        I.AO->Bias = s.AOBias;
        I.AO->Samples = s.AOSamples;
        I.Glow->Threshold = s.BloomThreshold;
        I.Glow->Knee = s.BloomKnee;
        I.Glow->Radius = s.BloomRadius;
        I.Glow->MaxBrightness = s.BloomMaxBrightness;
        VolFog = s.VolumetricFog;
    }

    void SceneRenderer::GatherSettings(SceneSettings& s) const {
        const Impl& I = *m_Impl;
        s.EnvironmentMap = CurrentEnvironment();
        s.ShowSky = I.Env->Show;
        s.SkyBlur = I.Env->Blur;
        s.SkyRotation = I.Env->RotationDegrees;
        s.Post = Post;
        s.Post.AODebug = false;                  // a debug view, not part of the level
        s.Shadows.CascadeDebug = false;          // likewise
        s.Shadows = I.Shadows.Settings;
        s.AORadius = I.AO->Radius;
        s.AOBias = I.AO->Bias;
        s.AOSamples = I.AO->Samples;
        s.BloomThreshold = I.Glow->Threshold;
        s.BloomKnee = I.Glow->Knee;
        s.BloomRadius = I.Glow->Radius;
        s.BloomMaxBrightness = I.Glow->MaxBrightness;
        s.VolumetricFog = VolFog;
    }

} // namespace Engine
