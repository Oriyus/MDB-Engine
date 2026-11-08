#pragma once
#include "Renderer/PostProcess.h"
#include "Renderer/Shadowmap.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    class Camera;
    class Shader;
    class Framebuffer;
    class Environment;
    class SSAO;
    class Bloom;
    struct SceneSettings;

    // The whole frame: shadow passes -> HDR scene (+ entity IDs for picking) -> skybox -> SSAO ->
    // bloom -> exposure / tone map / gamma -> FXAA. The result is an 8-bit texture (GetOutput) for
    // the app to show: the editor puts it in its Viewport panel, the game draws it full screen.
    // Owns every shader, render target and the environment (IBL). Needs a GL context.
    class SceneRenderer {
    public:
        // engineDir: the engine's files (Shaders/); assetDir: the project's Assets (Environments/). Both end in '/'.
        SceneRenderer(const std::string& engineDir, const std::string& assetDir, uint32_t width = 1280, uint32_t height = 720);
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Render at width x height (resizes the targets when the size changes). Call outside the
        // ImGui frame: it also bakes a requested environment first.
        void Render(Scene& scene, const Camera& camera, uint32_t width, uint32_t height);

        uint32_t GetOutput() const;               // final image (GL texture), bottom-up
        uint32_t GetWidth() const;
        uint32_t GetHeight() const;
        Framebuffer& GetFramebuffer();            // HDR color + entity ID + depth (for picking)

        void ReloadShaders();

        // Environment: "" = procedural sky, otherwise a .hdr file name in Assets/Environments.
        // The bake happens at the start of the next Render.
        void RequestEnvironment(const std::string& hdrFile);
        std::string CurrentEnvironment() const;   // the requested one if a bake is pending
        std::vector<std::string> ListEnvironments() const;
        Environment& GetEnvironment();

        // Copy the rendering parts of a level's settings in or out (environment, sky, post, AO,
        // bloom, shadows). Gravity and the editor camera are left alone.
        void ApplySettings(const SceneSettings& s);
        void GatherSettings(SceneSettings& s) const;

        PostSettings Post;
        bool ShowFog = true;                   // draw fog of war when a script turned it on (the editor can hide it)
        ShadowSettings& Shadows();
        SSAO& GetSSAO();
        Bloom& GetBloom();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
