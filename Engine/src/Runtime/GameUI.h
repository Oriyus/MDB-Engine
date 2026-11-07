#pragma once
#include "Scripting/ScriptEngine.h"

#include <imgui.h>
#include <vector>

namespace Engine {

    // A large TTF (Segoe UI Bold, or Arial Bold) for game text, so it stays crisp when scaled.
    // Call before the first ImGui frame (it adds to the font atlas). Keeps the default font first.
    // Returns null if no font file was found (the default font is used then).
    ImFont* LoadGameFont();

    // Draw the frame's ui.text calls into a rectangle (the viewport, or the whole window).
    // Sizes are given at 720p and scale with the rectangle's height.
    void DrawScriptText(ImDrawList* drawList, ImFont* font, const std::vector<ScriptText>& texts,
        const ImVec2& origin, const ImVec2& size);

} // namespace Engine
