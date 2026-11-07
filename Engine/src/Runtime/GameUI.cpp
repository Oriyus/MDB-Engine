#include "Runtime/GameUI.h"
#include "Renderer/Texture.h"
#include "Core/Log.h"

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

    const Texture2D* UITextureCache::Get(const std::string& path) {
        if (auto it = m_Textures.find(path); it != m_Textures.end()) return it->second.get();
        if (m_Failed.count(path)) return nullptr;
        auto tex = std::make_shared<Texture2D>(m_AssetDir + path, false);
        if (tex->GetID() == 0 || tex->GetWidth() == 0) {
            m_Failed.insert(path);
            ENGINE_WARN("ui.image: can't load {}", path);
            return nullptr;
        }
        return (m_Textures[path] = tex).get();
    }

    namespace {
        ImU32 ToColor(const glm::vec3& c, float alpha) {
            const glm::vec3 k = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
            return ImGui::ColorConvertFloat4ToU32(ImVec4(k.r, k.g, k.b, std::clamp(alpha, 0.0f, 1.0f)));
        }

        // Text with a soft drop shadow, aligned on x (0 left, 1 centre, 2 right)
        void DrawLabel(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, int align, const std::string& text,
            const glm::vec3& color, float alpha) {
            const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
            pos.x -= extent.x * 0.5f * (float)align;
            const float shadow = std::max(1.0f, px / 16.0f);
            dl->AddText(font, px, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, (int)(170 * std::clamp(alpha, 0.0f, 1.0f))), text.c_str());
            dl->AddText(font, px, pos, ToColor(color, alpha), text.c_str());
        }
    }

    bool UIButtonHovered(const ScriptUIItem& b, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse) {
        const float x = (mouse.x - origin.x) / size.x, y = (mouse.y - origin.y) / size.y;
        return x >= b.X && x <= b.X + b.W && y >= b.Y && y <= b.Y + b.H;
    }

    void DrawScriptUI(ImDrawList* dl, ImFont* font, const std::vector<ScriptUIItem>& items,
        const ImVec2& origin, const ImVec2& size, UITextureCache* textures) {
        if (!font) font = ImGui::GetFont();
        const float scale = size.y / 720.0f;               // sizes and roundings are given at 720p
        auto at = [&](float x, float y) { return ImVec2(origin.x + x * size.x, origin.y + y * size.y); };

        for (const auto& it : items) {
            switch (it.Kind) {
            case ScriptUIItem::Type::Text:
                DrawLabel(dl, font, std::clamp(it.Size * scale, 6.0f, 256.0f), at(it.X, it.Y), it.Align, it.Text, it.Color, it.Alpha);
                break;

            case ScriptUIItem::Type::Rect:
                dl->AddRectFilled(at(it.X, it.Y), at(it.X + it.W, it.Y + it.H), ToColor(it.Color, it.Alpha), it.Rounding * scale);
                break;

            case ScriptUIItem::Type::Image: {
                const Texture2D* tex = textures ? textures->Get(it.Text) : nullptr;
                if (!tex) break;
                float h = it.H;
                if (h <= 0.0f)                              // keep the image's aspect ratio on screen
                    h = it.W * size.x * (float)tex->GetHeight() / (float)tex->GetWidth() / size.y;
                // GL textures are stored bottom-up: flip V
                dl->AddImageRounded((ImTextureID)(intptr_t)tex->GetID(), at(it.X, it.Y), at(it.X + it.W, it.Y + h),
                    ImVec2(0, 1), ImVec2(1, 0), ToColor(it.Color, it.Alpha), it.Rounding * scale);
                break;
            }

            case ScriptUIItem::Type::Button: {
                glm::vec3 face = it.Color;
                if (it.Pressed)      face *= 0.75f;
                else if (it.Hovered) face = glm::mix(face, glm::vec3(1.0f), 0.18f);
                const ImVec2 p0 = at(it.X, it.Y), p1 = at(it.X + it.W, it.Y + it.H);
                dl->AddRectFilled(p0, p1, ToColor(face, it.Alpha), it.Rounding * scale);
                dl->AddRect(p0, p1, ToColor(glm::vec3(1.0f), (it.Hovered ? 0.55f : 0.25f) * it.Alpha), it.Rounding * scale, 0, std::max(1.0f, 2.0f * scale));
                const float px = std::clamp(it.Size * scale, 6.0f, 256.0f);
                const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0.0f, it.Text.c_str());
                DrawLabel(dl, font, px, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - extent.y * 0.5f), 1, it.Text, it.TextColor, it.Alpha);
                break;
            }
            }
        }
    }

} // namespace Engine
