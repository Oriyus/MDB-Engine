#include "Runtime/GameUI.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>

namespace Engine {

    ImFont* LoadGameFont() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
        for (const char* f : { "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf" })
            if (std::filesystem::exists(f)) return io.Fonts->AddFontFromFileTTF(f, 64.0f);
        return nullptr;
    }

    void DrawScriptText(ImDrawList* dl, ImFont* font, const std::vector<ScriptText>& texts,
        const ImVec2& origin, const ImVec2& size) {
        if (!font) font = ImGui::GetFont();
        for (const auto& t : texts) {
            const float px = std::clamp(t.Size * size.y / 720.0f, 6.0f, 256.0f);
            const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0.0f, t.Text.c_str());
            const ImVec2 pos(origin.x + t.X * size.x - extent.x * 0.5f * (float)t.Align, origin.y + t.Y * size.y);
            const glm::vec3 c = glm::clamp(t.Color, glm::vec3(0.0f), glm::vec3(1.0f));
            const float shadow = std::max(1.0f, px / 16.0f);
            dl->AddText(font, px, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 170), t.Text.c_str());
            dl->AddText(font, px, pos, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), t.Text.c_str());
        }
    }

} // namespace Engine
