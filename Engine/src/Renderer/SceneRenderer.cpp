#include "Renderer/SceneRenderer.h"
#include "Renderer/Shader.h"
#include "Renderer/DrawList.h"
#include "Renderer/Camera.h"
#include "Renderer/Framebuffer.h"
#include "Renderer/Environment.h"
#include "Renderer/SSAO.h"
#include "Renderer/Bloom.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"

#include <algorithm>
#include <filesystem>

namespace Engine {

    struct SceneRenderer::Impl {
        std::string AssetDir;
        std::unique_ptr<Shader> Lit, PointDepth, SunDepth, Post, SSAOShader, SSAOBlur, BloomDown, BloomUp, FXAA;
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

        std::string ShaderPath(const char* name) const { return AssetDir + "Shaders/" + name; }

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

    SceneRenderer::SceneRenderer(const std::string& assetDir, uint32_t width, uint32_t height)
        : m_Impl(std::make_unique<Impl>()) {
        Impl& I = *m_Impl;
        I.AssetDir = assetDir;
        I.Lit = std::make_unique<Shader>(I.ShaderPath("lit.vert"), I.ShaderPath("lit.frag"));
        I.PointDepth = std::make_unique<Shader>(I.ShaderPath("shadow.vert"), I.ShaderPath("shadow.frag"));
        I.SunDepth = std::make_unique<Shader>(I.ShaderPath("shadow.vert"), I.ShaderPath("sun_shadow.frag"));
        I.Post = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("tonemap.frag"));
        I.SSAOShader = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("ssao.frag"));
        I.SSAOBlur = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("ssao_blur.frag"));
        I.BloomDown = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("bloom_down.frag"));
        I.BloomUp = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("bloom_up.frag"));
        I.FXAA = std::make_unique<Shader>(I.ShaderPath("tonemap.vert"), I.ShaderPath("fxaa.frag"));

        I.Env = std::make_unique<Environment>(assetDir + "Shaders/");
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

        // 1) Depth passes (all point lights + sun)
        scene.BuildDrawList(*I.Draws, camera, &I.Shadows);
        scene.RenderShadows(I.Shadows, *I.Draws);

        // 2) Scene into the HDR framebuffer, then the sky behind it
        I.HDR->Bind();
        I.HDR->Clear(0.005f, 0.005f, 0.01f, 1.0f);
        scene.Render(camera, *I.Lit, *I.Draws, &I.Shadows, I.Env.get());
        I.Env->DrawSkybox(camera, scene.AmbientIntensity);
        scene.Render(camera, *I.Lit, *I.Draws, &I.Shadows, I.Env.get(), Scene::RenderPass::Transparent);   // glass etc. over the sky
        Framebuffer::Unbind();

        // 3) Screen-space effects
        uint32_t aoTex = 0, bloomTex = 0;
        if (Post.AOEnabled) {
            I.AO->Resize(width, height);
            I.AO->Compute(I.HDR->GetDepthAttachment(), camera.GetProjection(), *I.SSAOShader, *I.SSAOBlur);
            aoTex = I.AO->GetOutput();
        }
        if (Post.BloomEnabled) {
            I.Glow->Resize(width, height);
            I.Glow->Compute(I.HDR->GetColorAttachment(), *I.BloomDown, *I.BloomUp);
            bloomTex = I.Glow->GetOutput();
        }

        // 4) AO + bloom + exposure + tone map + gamma into an 8-bit texture, then FXAA
        I.Final->Resize(width, height);
        I.Final->Apply(I.HDR->GetColorAttachment(), aoTex, bloomTex, *I.Post, Post);
        I.Final->ApplyFXAA(*I.FXAA, Post.FXAA);
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
                         I.SSAOBlur.get(), I.BloomDown.get(), I.BloomUp.get(), I.FXAA.get() })
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
    }

} // namespace Engine
