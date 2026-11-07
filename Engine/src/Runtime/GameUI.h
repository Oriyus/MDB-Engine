#pragma once
#include "Scripting/ScriptEngine.h"

#include <imgui.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

    class Texture2D;

    // A large TTF (Segoe UI Bold, or Arial Bold) for game text, so it stays crisp when scaled.
    // Call before the first ImGui frame (it adds to the font atlas). Keeps the default font first.
    // Returns null if no font file was found (the default font is used then).
    ImFont* LoadGameFont();

    // Images for ui.image, loaded on first use from the asset folder and kept. Loaded as plain
    // (not sRGB) textures: ImGui draws straight to the screen, so the pixels go through unchanged.
    class UITextureCache {
    public:
        explicit UITextureCache(std::string assetDir) : m_AssetDir(std::move(assetDir)) {}
        const Texture2D* Get(const std::string& path);   // null if it can't be loaded (warns once)
    private:
        std::string m_AssetDir;
        std::unordered_map<std::string, std::shared_ptr<Texture2D>> m_Textures;
        std::unordered_set<std::string> m_Failed;
    };

    // Draw a frame's ui.* items into a rectangle (the viewport, or the whole window). Sizes are
    // given at 720p and scale with the rectangle's height. `textures` may be null (no images then).
    void DrawScriptUI(ImDrawList* drawList, ImFont* font, const std::vector<ScriptUIItem>& items,
        const ImVec2& origin, const ImVec2& size, UITextureCache* textures);

    // Text only (used for simple overlays)
    inline void DrawScriptText(ImDrawList* drawList, ImFont* font, const std::vector<ScriptUIItem>& items,
        const ImVec2& origin, const ImVec2& size) {
        DrawScriptUI(drawList, font, items, origin, size, nullptr);
    }

    // Hit test for a button laid out like ui.button (x, y, w, h in 0..1 of the rectangle)
    bool UIButtonHovered(const ScriptUIItem& button, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse);

} // namespace Engine
