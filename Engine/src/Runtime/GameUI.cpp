#include "Runtime/GameUI.h"
#include "Renderer/Texture.h"
#include "Core/Log.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>

namespace Engine {

    UIFonts LoadGameFonts() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
        auto load = [&](std::initializer_list<const char*> files) -> ImFont* {
            for (const char* f : files)
                if (std::filesystem::exists(f)) return io.Fonts->AddFontFromFileTTF(f, 64.0f);
            return nullptr;
        };
        UIFonts fonts;
        fonts.Bold = load({ "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf" });
        fonts.Regular = load({ "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" });
        if (!fonts.Regular) fonts.Regular = fonts.Bold;
        if (!fonts.Bold) fonts.Bold = fonts.Regular;
        return fonts;
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

        // Text, optionally with a soft drop shadow, aligned on x (0 left, 1 centre, 2 right)
        void DrawLabel(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, int align, const std::string& text,
            const glm::vec3& color, float alpha, bool shadow = true) {
            const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
            pos.x -= extent.x * 0.5f * (float)align;
            if (shadow) {
                const float d = std::max(1.0f, px / 16.0f);
                dl->AddText(font, px, ImVec2(pos.x + d, pos.y + d), IM_COL32(0, 0, 0, (int)(170 * std::clamp(alpha, 0.0f, 1.0f))), text.c_str());
            }
            dl->AddText(font, px, pos, ToColor(color, alpha), text.c_str());
        }

        // An image in nine parts: the corners keep their size, the edges stretch one way, the middle both.
        // `border` texture pixels on each side are drawn `screenBorder` pixels wide.
        void DrawNineSlice(ImDrawList* dl, ImTextureID tex, ImVec2 p0, ImVec2 p1, float texW, float texH,
            float border, float screenBorder, ImU32 tint) {
            screenBorder = std::min(screenBorder, std::min(p1.x - p0.x, p1.y - p0.y) * 0.5f);
            const float xs[4] = { p0.x, p0.x + screenBorder, p1.x - screenBorder, p1.x };
            const float ys[4] = { p0.y, p0.y + screenBorder, p1.y - screenBorder, p1.y };
            const float us[4] = { 0.0f, border / texW, 1.0f - border / texW, 1.0f };
            const float vs[4] = { 0.0f, border / texH, 1.0f - border / texH, 1.0f };
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    // GL textures are stored bottom-up: flip V
                    dl->AddImage(tex, ImVec2(xs[c], ys[r]), ImVec2(xs[c + 1], ys[r + 1]),
                        ImVec2(us[c], 1.0f - vs[r]), ImVec2(us[c + 1], 1.0f - vs[r + 1]), tint);
        }
    }

    bool UIButtonHovered(const ScriptUIItem& b, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse) {
        const float x = (mouse.x - origin.x) / size.x, y = (mouse.y - origin.y) / size.y;
        return x >= b.X && x <= b.X + b.W && y >= b.Y && y <= b.Y + b.H;
    }

    void DrawScriptUI(ImDrawList* dl, const UIFonts& fonts, const std::vector<ScriptUIItem>& items,
        const ImVec2& origin, const ImVec2& size, UITextureCache* textures) {
        ImFont* bold = fonts.Bold ? fonts.Bold : (fonts.Regular ? fonts.Regular : ImGui::GetFont());
        ImFont* regular = fonts.Regular ? fonts.Regular : bold;
        const float scale = size.y / 720.0f;               // sizes and roundings are given at 720p
        auto at = [&](float x, float y) { return ImVec2(origin.x + x * size.x, origin.y + y * size.y); };
        int clips = 0;

        for (const auto& it : items) {
            switch (it.Kind) {
            case ScriptUIItem::Type::Text:
                DrawLabel(dl, it.Regular ? regular : bold, std::clamp(it.Size * scale, 4.0f, 256.0f), at(it.X, it.Y),
                    it.Align, it.Text, it.Color, it.Alpha, it.Shadow);
                break;

            case ScriptUIItem::Type::Rect:
                dl->AddRectFilled(at(it.X, it.Y), at(it.X + it.W, it.Y + it.H), ToColor(it.Color, it.Alpha), it.Rounding * scale);
                break;

            case ScriptUIItem::Type::Border:
                dl->AddRect(at(it.X, it.Y), at(it.X + it.W, it.Y + it.H), ToColor(it.Color, it.Alpha), it.Rounding * scale,
                    0, std::max(1.0f, it.Thickness * scale));
                break;

            case ScriptUIItem::Type::ClipPush:
                dl->PushClipRect(at(it.X, it.Y), at(it.X + it.W, it.Y + it.H), true);
                clips++;
                break;

            case ScriptUIItem::Type::ClipPop:
                if (clips > 0) { dl->PopClipRect(); clips--; }
                break;

            case ScriptUIItem::Type::Image: {
                const Texture2D* tex = textures ? textures->Get(it.Text) : nullptr;
                if (!tex) break;
                float h = it.H;
                if (h <= 0.0f)                              // keep the image's aspect ratio on screen
                    h = it.W * size.x * (float)tex->GetHeight() / (float)tex->GetWidth() / size.y;
                const ImTextureID id = (ImTextureID)(intptr_t)tex->GetID();
                if (it.Slice > 0.0f) {
                    DrawNineSlice(dl, id, at(it.X, it.Y), at(it.X + it.W, it.Y + h), (float)tex->GetWidth(),
                        (float)tex->GetHeight(), it.Slice, it.Slice * scale, ToColor(it.Color, it.Alpha));
                    break;
                }
                // GL textures are stored bottom-up: flip V
                dl->AddImageRounded(id, at(it.X, it.Y), at(it.X + it.W, it.Y + h),
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
                const float px = std::clamp(it.Size * scale, 4.0f, 256.0f);
                const ImVec2 extent = bold->CalcTextSizeA(px, FLT_MAX, 0.0f, it.Text.c_str());
                DrawLabel(dl, bold, px, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - extent.y * 0.5f), 1, it.Text, it.TextColor, it.Alpha, it.Shadow);
                break;
            }
            }
        }
        while (clips-- > 0) dl->PopClipRect();     // a script that left a clip open
    }

} // namespace Engine
