#include "Runtime/UILayout.h"
#include "Core/Input.h"

#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace Engine {

    namespace {
        constexpr float kUnitH = 720.0f;            // the view is 720 units tall
        constexpr size_t kMaxItems = 8192;
        constexpr float kLineSpacing = 1.25f;       // line height = text size * this
        constexpr float kDragThreshold = 6.0f;      // units the pointer moves before a press becomes a drag
        constexpr float kTooltipDelay = 0.35f;

        bool ParseHex(const std::string& s, glm::vec3& out) {
            if (s.size() != 6) return false;
            unsigned v = 0;
            for (char c : s) {
                v <<= 4;
                if (c >= '0' && c <= '9') v |= unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
                else return false;
            }
            out = glm::vec3(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f);
            return true;
        }

        // A run of text with one style. SpaceBefore: a space separates it from the piece before.
        struct Piece {
            std::string Text;
            glm::vec3 Color{ 1.0f };
            bool Bold = false;
            bool SpaceBefore = false;
            bool Newline = false;           // a line break (Text empty)
            float Width = 0.0f;
        };

        // Markup: {#rrggbb} colour, {/} back to the default colour, {b} ... {/b} bold. Anything else in
        // braces is kept as text.
        std::vector<Piece> ParseMarkup(const std::string& text, const glm::vec3& color, bool bold) {
            std::vector<Piece> out;
            Piece cur;
            cur.Color = color;
            cur.Bold = bold;
            bool space = false;
            auto flush = [&]() {
                if (cur.Text.empty()) return;
                cur.SpaceBefore = space;
                space = false;
                out.push_back(cur);
                cur.Text.clear();
            };
            for (size_t i = 0; i < text.size(); i++) {
                const char c = text[i];
                if (c == ' ' || c == '\t') { flush(); space = true; continue; }
                if (c == '\n') {
                    flush();
                    Piece nl;
                    nl.Newline = true;
                    out.push_back(nl);
                    space = false;
                    continue;
                }
                if (c == '{') {
                    const size_t end = text.find('}', i);
                    if (end != std::string::npos && end - i <= 8) {
                        const std::string tag = text.substr(i + 1, end - i - 1);
                        glm::vec3 col;
                        bool known = true;
                        if (tag == "/") { flush(); cur.Color = color; }
                        else if (tag == "b") { flush(); cur.Bold = true; }
                        else if (tag == "/b") { flush(); cur.Bold = bold; }
                        else if (!tag.empty() && tag[0] == '#' && ParseHex(tag.substr(1), col)) { flush(); cur.Color = col; }
                        else known = false;
                        if (known) {
                            i = end;
                            continue;
                        }
                    }
                }
                cur.Text += c;
            }
            flush();
            return out;
        }

        // ---- UTF-8 for text fields: offsets are bytes, always on a code point's first byte
        bool IsCont(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
        size_t NextCp(const std::string& s, size_t i) {
            if (i >= s.size()) return s.size();
            i++;
            while (i < s.size() && IsCont(s[i])) i++;
            return i;
        }
        size_t PrevCp(const std::string& s, size_t i) {
            if (i == 0) return 0;
            i = std::min(i, s.size()) - 1;
            while (i > 0 && IsCont(s[i])) i--;
            return i;
        }
        size_t CountCp(const std::string& s) {
            size_t n = 0;
            for (char c : s) if (!IsCont(c)) n++;
            return n;
        }
        std::string EncodeCp(uint32_t cp) {
            std::string o;
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x110000) {
                o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
                o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
            }
            return o;
        }
        // Decodes the code point at i (bad bytes count as themselves)
        uint32_t DecodeCp(const std::string& s, size_t i) {
            const unsigned char c = (unsigned char)s[i];
            const size_t end = NextCp(s, i);
            if (c < 0x80 || end - i < 2) return c;
            uint32_t cp = c & (end - i == 2 ? 0x1F : end - i == 3 ? 0x0F : 0x07);
            for (size_t k = i + 1; k < end; k++) cp = (cp << 6) | ((unsigned char)s[k] & 0x3F);
            return cp;
        }
        bool IsSpaceCp(const std::string& s, size_t i) { return i < s.size() && (s[i] == ' ' || s[i] == '\t'); }
        // Ctrl+Left / Ctrl+Right: to the start of this or the previous word / past the end of this or the next one
        size_t WordLeft(const std::string& s, size_t i) {
            while (i > 0 && IsSpaceCp(s, PrevCp(s, i))) i = PrevCp(s, i);
            while (i > 0 && !IsSpaceCp(s, PrevCp(s, i))) i = PrevCp(s, i);
            return i;
        }
        size_t WordRight(const std::string& s, size_t i) {
            while (i < s.size() && !IsSpaceCp(s, i)) i = NextCp(s, i);
            while (i < s.size() && IsSpaceCp(s, i)) i = NextCp(s, i);
            return i;
        }
        bool FilterAllows(UIInputFilter f, uint32_t cp) {
            const bool digit = cp >= '0' && cp <= '9';
            switch (f) {
            case UIInputFilter::Integer: return digit || cp == '-' || cp == '+';
            case UIInputFilter::Decimal: return digit || cp == '-' || cp == '+' || cp == '.' || cp == ',';
            case UIInputFilter::Name:    return digit || cp == '_' || cp == '-' || cp > 127 ||
                                                (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
            default:                     return cp >= 32 && cp != 127;
            }
        }
        // GLFW key codes the text field handles
        namespace K {
            constexpr int A = 65, C = 67, V = 86, X = 88;
            constexpr int Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Delete = 261;
            constexpr int Right = 262, Left = 263, Down = 264, Up = 265, Home = 268, End = 269, KeypadEnter = 335;
        }
    }

    // ------------------------------------------------------------------ frame
    void UIContext::Reset() {
        m_Panels.clear(); m_PrevPanels.clear();
        m_Blockers.clear(); m_PrevBlockers.clear();
        m_PanelHeight.clear();
        m_ScrollOffset.clear(); m_ScrollContent.clear();
        m_ActiveId.clear(); m_RightActiveId.clear();
        m_Dragging = m_DragEnded = m_DragEndedLast = false;
        m_DragId.clear(); m_DragIcon.clear();
        m_HotId.clear(); m_PrevHotId.clear();
        m_HotTime = 0.0f;
        m_PrevDown = m_PrevRightDown = false;
        m_FocusId.clear(); m_FocusRequest.clear();
        m_FocusAtBegin = false;
        m_TabMove = 0;
        m_InputOrder.clear();
        m_FocusAsks.clear(); m_PrevFocusAsks.clear();
        m_TextEvents.clear();
        m_NavIn = m_NavPrev = UINavInput{};
        for (float& t : m_NavRepeat) t = 0.0f;
        m_NavItems.clear(); m_PrevNavItems.clear();
        m_NavId.clear(); m_NavActivate.clear(); m_NavSecondary.clear(); m_NavScrollTo.clear(); m_NavPreferPanel.clear();
        m_NavAdjust = 0;
        m_NavVisible = m_NavPickDefault = m_NavCarry = m_NavSeen = m_HotFromNav = m_PtrMoved = false;
        m_BackPressed = m_BackUsed = m_WantsBack = m_BackAsked = false;
        m_BackPanel.clear();
        m_ClosablePanels.clear();
        m_PrevPX = m_PrevPY = -1e6f;
        Theme = UITheme{};
    }

    void UIContext::BeginFrame(std::vector<ScriptUIItem>& out, float aspect, const UIPointerState& p, float dt) {
        m_Out = &out;
        m_VW = kUnitH * std::max(aspect, 0.1f);
        m_Dt = dt;
        m_PX = p.X * m_VW;
        m_PY = p.Y * kUnitH;
        m_Inside = p.Inside;
        m_Wheel = p.Inside ? p.Wheel : 0.0f;
        m_Pressed = p.Down && !m_PrevDown && p.Inside;
        m_Released = !p.Down && m_PrevDown;
        m_Down = p.Down;
        m_PrevDown = p.Down;
        m_RightPressed = p.RightDown && !m_PrevRightDown && p.Inside;
        m_RightDown = p.RightDown;
        m_PrevRightDown = p.RightDown;

        m_PrevPanels = std::move(m_Panels);
        m_Panels.clear();
        m_PrevBlockers = std::move(m_Blockers);
        m_Blockers.clear();
        m_HoverPanel.clear();
        if (m_Inside)
            for (auto it = m_PrevPanels.rbegin(); it != m_PrevPanels.rend(); ++it)
                if (it->second.Contains(m_PX, m_PY)) { m_HoverPanel = it->first; break; }

        m_Stack.clear();
        m_Clips.clear();
        m_IdCount.clear();
        m_HotId.clear();
        m_PendingTooltip.clear();
        m_Last = Rect{};
        m_LastId.clear();
        m_DragEndedLast = m_DragEnded;
        m_DragEnded = false;

        m_FocusAtBegin = !m_FocusId.empty();
        m_FocusSeen = false;
        m_PressOnFocus = false;
        m_TabMove = 0;
        m_InputOrder.clear();
        m_PrevFocusAsks.swap(m_FocusAsks);
        m_FocusAsks.clear();
        if (!m_FocusId.empty()) m_Blink += dt;
        m_HotFromNav = false;
        NavBegin();
    }

    int UIContext::EndFrame() {
        int open = 0;
        while (!m_Stack.empty()) {
            open++;
            CloseTop();
        }

        // Tooltip: after the pointer rested on the same widget for a moment, on top of everything
        if (!m_HotId.empty() && m_HotId == m_PrevHotId) m_HotTime += m_Dt;
        else m_HotTime = 0.0f;
        m_PrevHotId = m_HotId;
        if (!m_PendingTooltip.empty() && m_HotTime >= kTooltipDelay && !m_Dragging && m_Out) {
            const float pad = 10.0f, size = Theme.TextSize * 0.9f;
            UITextOptions o;
            o.Size = size;
            const float natural = LayoutText(m_PendingTooltip, 0, 0, 0, o, false).x;
            const float textW = std::min(natural, Theme.TooltipWidth - 2 * pad);
            const glm::vec2 m = LayoutText(m_PendingTooltip, 0, 0, textW, o, false);
            // At the pointer, or below the widget with the navigation focus
            const float ax = m_HotFromNav ? m_NavRect.X + m_NavRect.W * 0.5f - 18.0f : m_PX;
            const float ay = m_HotFromNav ? m_NavRect.Y + m_NavRect.H - 14.0f : m_PY;
            Rect r{ ax + 18.0f, ay + 22.0f, textW + 2 * pad, m.y + 2 * pad };
            if (r.X + r.W > m_VW - 4) r.X = ax - r.W - 8;
            if (r.Y + r.H > kUnitH - 4) r.Y = (m_HotFromNav ? m_NavRect.Y : ay) - r.H - 8;
            r.X = std::max(4.0f, r.X);
            EmitFrame(r, Theme.PanelColor * 0.8f, 0.97f, std::string(), 0.0f, 6.0f);
            LayoutText(m_PendingTooltip, r.X + pad, r.Y + pad, textW, o, true);
        }
        // Text fields: the focused one wasn't drawn (its panel closed) or a press landed elsewhere: it lets go.
        // Tab / Shift+Tab: the next / previous field drawn this frame takes the focus next frame.
        if (!m_FocusId.empty() && (!m_FocusSeen || (m_Pressed && !m_PressOnFocus))) m_FocusId.clear();
        m_FocusRequest.clear();                  // a Tab target that wasn't drawn this frame is forgotten
        if (m_TabMove != 0 && !m_InputOrder.empty()) {
            const auto it = std::find(m_InputOrder.begin(), m_InputOrder.end(), m_FocusId);
            const int n = (int)m_InputOrder.size();
            int i = it == m_InputOrder.end() ? (m_TabMove > 0 ? -1 : 0) : (int)(it - m_InputOrder.begin());
            i = ((i + m_TabMove) % n + n) % n;
            m_FocusRequest = m_InputOrder[(size_t)i];
            m_FocusId.clear();
            m_NavId = m_FocusRequest;            // navigation goes on from there
        }
        m_TabMove = 0;
        m_TextEvents.clear();

        // The dragged icon follows the pointer (carried by navigation: it sits on the focused widget's corner)
        if (m_Dragging && !m_DragIcon.empty() && m_Out) {
            if (m_NavCarry) EmitImage(m_DragIcon, m_NavRect.X + m_NavRect.W * 0.45f, m_NavRect.Y - 16.0f, 48.0f, 48.0f, glm::vec3(1.0f), 0.95f, 0.0f);
            else EmitImage(m_DragIcon, m_PX - 28.0f, m_PY - 28.0f, 56.0f, 56.0f, glm::vec3(1.0f), 0.9f, 0.0f);
        }

        if (m_Released) {
            if (m_Dragging) m_DragEnded = true;
            m_Dragging = m_NavCarry = false;
            m_DragId.clear();
            m_DragIcon.clear();
            m_ActiveId.clear();
        }
        NavEnd();
        m_Out = nullptr;
        return open;
    }

    // ------------------------------------------------------------------ emitting
    ScriptUIItem& UIContext::Emit(ScriptUIItem::Type type, float x, float y, float w, float h) {
        static ScriptUIItem overflow;
        if (!m_Out || m_Out->size() >= kMaxItems) { overflow = ScriptUIItem{}; return overflow; }
        ScriptUIItem it;
        it.Kind = type;
        it.X = x / m_VW;
        it.Y = y / kUnitH;
        it.W = w / m_VW;
        it.H = h / kUnitH;
        m_Out->push_back(std::move(it));
        return m_Out->back();
    }

    void UIContext::EmitText(const std::string& text, float x, float y, float size, const glm::vec3& color, float alpha,
        bool bold, bool shadow, int align) {
        ScriptUIItem& t = Emit(ScriptUIItem::Type::Text, x, y, 0, 0);
        t.Text = text;
        t.Size = size;
        t.Color = color;
        t.Alpha = alpha;
        t.Regular = !bold;
        t.Shadow = shadow;
        t.Align = align;
    }

    void UIContext::EmitImage(const std::string& path, float x, float y, float w, float h, const glm::vec3& color,
        float alpha, float slice) {
        ScriptUIItem& t = Emit(ScriptUIItem::Type::Image, x, y, w, h);
        t.Text = path;
        t.Color = color;
        t.Alpha = alpha;
        t.Slice = slice;
    }

    void UIContext::EmitFrame(const Rect& r, const glm::vec3& color, float alpha, const std::string& image, float slice, float rounding) {
        if (!image.empty()) {
            EmitImage(image, r.X, r.Y, r.W, r.H, color, alpha, slice);
            return;
        }
        ScriptUIItem& f = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
        f.Color = color;
        f.Alpha = alpha;
        f.Rounding = rounding;
        ScriptUIItem& b = Emit(ScriptUIItem::Type::Border, r.X, r.Y, r.W, r.H);
        b.Color = Theme.BorderColor;
        b.Alpha = Theme.BorderAlpha * std::min(1.0f, alpha + 0.1f);
        b.Rounding = rounding;
    }

    // ------------------------------------------------------------------ layout helpers
    UIContext::Rect UIContext::Place(float w, float h) {
        if (m_Stack.empty()) throw std::runtime_error("call it between ui.begin_panel and ui.end_panel");
        Box& b = m_Stack.back();
        if (b.K == Box::Kind::Row) {
            Rect r{ b.CurX, b.Y, w, h };
            b.Children.emplace_back(m_Out ? m_Out->size() : 0, h);
            b.CurX += w + b.Spacing;
            b.LineH = std::max(b.LineH, h);
            b.MaxRight = std::max(b.MaxRight, r.X + w);
            return r;
        }
        Rect r{ b.X, b.CurY, w, h };
        b.CurY += h + b.Spacing;
        b.MaxRight = std::max(b.MaxRight, r.X + w);
        return r;
    }

    void UIContext::ChildDone(float x, float y, float w, float h, size_t firstItem) {
        if (m_Stack.empty()) return;
        Box& parent = m_Stack.back();
        if (parent.K == Box::Kind::Row) {
            parent.Children.emplace_back(firstItem, h);
            parent.CurX += w + parent.Spacing;
            parent.LineH = std::max(parent.LineH, h);
        }
        else {
            parent.CurY += h + parent.Spacing;
        }
        parent.MaxRight = std::max(parent.MaxRight, x + w);
        m_Last = Rect{ x, y, w, h };
    }

    void UIContext::CloseTop() {
        switch (m_Stack.back().K) {
        case Box::Kind::Panel:  EndPanel(); break;
        case Box::Kind::Row:    EndRow(); break;
        case Box::Kind::Scroll: EndScroll(); break;
        case Box::Kind::Column: EndColumn(); break;
        }
    }

    float UIContext::Avail() const {
        if (m_Stack.empty()) return 0.0f;
        const Box& b = m_Stack.back();
        if (b.K == Box::Kind::Row) return std::max(0.0f, b.X + b.W - b.CurX);
        return b.W;
    }

    UIContext::Rect UIContext::Clip() const {
        if (m_Clips.empty()) return Rect{ -1e6f, -1e6f, 2e6f, 2e6f };
        return m_Clips.back();
    }

    const std::string& UIContext::CurrentPanel() const {
        static const std::string none;
        for (auto it = m_Stack.rbegin(); it != m_Stack.rend(); ++it)
            if (it->K == Box::Kind::Panel) return it->Id;
        return none;
    }

    bool UIContext::Hover(const Rect& r) const {
        return m_Inside && r.Contains(m_PX, m_PY) && Clip().Contains(m_PX, m_PY) && CurrentPanel() == m_HoverPanel;
    }

    std::string UIContext::MakeId(const std::string& base) {
        const std::string key = CurrentPanel() + "/" + base;
        const int n = m_IdCount[key]++;
        return n == 0 ? key : key + "#" + std::to_string(n);
    }

    ImFont* UIContext::FontFor(bool bold) {
        ImFont* f = bold ? (m_Bold ? m_Bold : m_Regular) : (m_Regular ? m_Regular : m_Bold);
        if (!f && ImGui::GetCurrentContext() && !ImGui::GetIO().Fonts->Fonts.empty()) f = ImGui::GetIO().Fonts->Fonts[0];
        // Scripts may run before the first ImGui frame has built the atlas: build it now
        if (f && f->ContainerAtlas && !f->ContainerAtlas->IsBuilt()) f->ContainerAtlas->Build();
        return f;
    }

    float UIContext::Measure(const std::string& text, float size, bool bold) {
        if (text.empty()) return 0.0f;
        ImFont* f = FontFor(bold);
        if (!f) return 0.5f * size * (float)text.size();
        return f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    }

    float UIContext::TextWidth(const std::string& text, float size, bool bold) {
        UITextOptions o;
        o.Size = size;
        o.Bold = bold;
        return LayoutText(text, 0, 0, 0, o, false).x;
    }

    // Lays text out from (x, y), wrapping at `width` (0: only at line breaks). Returns the widest
    // line and the height. emit: also write the text items (lines aligned within `width`).
    glm::vec2 UIContext::LayoutText(const std::string& text, float x, float y, float width, const UITextOptions& o, bool emit) {
        const float size = o.Size > 0.0f ? o.Size : Theme.TextSize;
        const glm::vec3 base = o.HasColor ? o.Color : (o.Muted ? Theme.MutedColor : Theme.TextColor);
        std::vector<Piece> pieces = ParseMarkup(text, base, o.Bold);
        if (pieces.empty()) return glm::vec2(0.0f);
        for (Piece& p : pieces) p.Width = p.Newline ? 0.0f : Measure(p.Text, size, p.Bold);
        const float space = Measure(" ", size, false);
        const float lineH = size * kLineSpacing;

        // Lines of piece indices; a "word" is a piece plus the pieces glued to it (no space between)
        std::vector<std::vector<size_t>> lines(1);
        std::vector<float> widths(1, 0.0f);
        for (size_t i = 0; i < pieces.size();) {
            if (pieces[i].Newline) { lines.emplace_back(); widths.push_back(0.0f); i++; continue; }
            size_t j = i + 1;
            float w = pieces[i].Width;
            while (j < pieces.size() && !pieces[j].Newline && !pieces[j].SpaceBefore) w += pieces[j++].Width;
            const float gap = lines.back().empty() ? 0.0f : space;
            if (width > 0.0f && !lines.back().empty() && widths.back() + gap + w > width) {
                lines.emplace_back();
                widths.push_back(0.0f);
            }
            const bool first = lines.back().empty();
            for (size_t k = i; k < j; k++) {
                if (k == i && first) pieces[k].SpaceBefore = false;     // no space at the start of a line
                lines.back().push_back(k);
            }
            widths.back() += (first ? 0.0f : space) + w;
            i = j;
        }

        float maxW = 0.0f;
        for (float w : widths) maxW = std::max(maxW, w);
        if (emit) {
            const float boxW = width > 0.0f ? width : maxW;
            for (size_t l = 0; l < lines.size(); l++) {
                float cx = x + (boxW - widths[l]) * 0.5f * (float)o.Align;
                const float cy = y + lineH * (float)l + (lineH - size) * 0.5f;
                // Merge neighbours of one style into one item
                std::string run;
                float runX = cx;
                const Piece* style = nullptr;
                auto flushRun = [&]() {
                    if (style && !run.empty())
                        EmitText(run, runX, cy, size, style->Color, o.Alpha, style->Bold, o.Shadow);
                    run.clear();
                };
                for (size_t k : lines[l]) {
                    const Piece& p = pieces[k];
                    const float gap = p.SpaceBefore ? space : 0.0f;
                    if (style && style->Color == p.Color && style->Bold == p.Bold) {
                        if (p.SpaceBefore) run += ' ';
                        run += p.Text;
                    }
                    else {
                        flushRun();
                        style = &p;
                        runX = cx + gap;
                        run = p.Text;
                    }
                    cx += gap + p.Width;
                }
                flushRun();
            }
        }
        return glm::vec2(maxW, lineH * (float)lines.size());
    }

    // ------------------------------------------------------------------ containers
    bool UIContext::BeginPanel(const std::string& id, const UIPanelOptions& o) {
        const float pad = o.Padding >= 0.0f ? o.Padding : Theme.Padding;
        // Auto sizes: as big as last frame (they can't be known before the content is laid out)
        const bool autoH = o.H <= 0.0f, autoW = o.W <= 0.0f;
        float h = o.H, w = o.W;
        if (autoH) { auto it = m_PanelHeight.find(id); h = it != m_PanelHeight.end() ? it->second : 0.0f; }
        if (autoW) { auto it = m_PanelWidth.find(id); w = it != m_PanelWidth.end() ? it->second : 0.0f; }
        Rect r{ o.X * m_VW - w * o.Pivot.x, o.Y * kUnitH - h * o.Pivot.y, w, h };

        Box b;
        b.K = Box::Kind::Panel;
        b.Id = id;
        b.Outer = r;
        b.Padding = pad;
        b.AutoH = autoH;
        b.AutoW = autoW;
        b.X = r.X + pad;
        b.MaxRight = b.X;
        b.W = autoW ? m_VW : std::max(0.0f, w - 2.0f * pad);
        b.Y = b.CurY = b.StartY = r.Y + pad;
        b.Spacing = Theme.Spacing;
        const size_t before = m_Out ? m_Out->size() : 0;
        const std::string& image = !o.Image.empty() ? o.Image : Theme.PanelImage;
        const float slice = o.Slice >= 0.0f ? o.Slice : Theme.PanelSlice;
        const glm::vec3 color = o.HasColor ? o.Color : (image.empty() ? Theme.PanelColor : glm::vec3(1.0f));
        const float alpha = o.Alpha >= 0.0f ? o.Alpha : (image.empty() ? Theme.PanelAlpha : 1.0f);   // framed: opaque
        EmitFrame(r, color, alpha, image, slice, Theme.Rounding);
        if (m_Out) for (size_t i = before; i < m_Out->size(); i++) b.Patch.push_back(i);
        m_Stack.push_back(std::move(b));
        m_Panels.emplace_back(id, r);

        bool open = true;
        Box& top = m_Stack.back();
        if (o.Close) {
            m_ClosablePanels.push_back(id);
            if (m_BackPressed && !m_BackUsed && m_BackPanel == id) { open = false; m_BackUsed = true; }   // Back closes it
            const float s = 26.0f;
            const Rect cr{ r.X + r.W - pad - s + 4.0f, r.Y + pad - 4.0f, s, s };
            const std::string cid = MakeId("__close");
            const bool hovered = !m_Dragging && Hover(cr);
            if (hovered && m_Pressed) m_ActiveId = cid;
            if (hovered && m_Released && m_ActiveId == cid) open = false;
            EmitText("x", cr.X + s * 0.5f, cr.Y + (s - 22.0f) * 0.5f - 2.0f, 22.0f,
                hovered ? glm::vec3(1.0f) : Theme.MutedColor, 1.0f, true, false, 1);
        }
        if (!o.Title.empty()) {
            UITextOptions t;
            t.Size = Theme.TitleSize;
            t.Bold = true;
            t.HasColor = true;
            t.Color = Theme.TitleColor;
            const glm::vec2 tm = LayoutText(o.Title, top.X, top.CurY, autoW ? 0.0f : top.W - (o.Close ? 30.0f : 0.0f), t, true);
            top.MaxRight = std::max(top.MaxRight, top.X + tm.x + (o.Close ? 30.0f : 0.0f));
            top.CurY += tm.y + 2.0f;
            const float lineW = autoW ? std::max(0.0f, w - 2.0f * pad) : top.W;
            ScriptUIItem& line = Emit(ScriptUIItem::Type::Rect, top.X, top.CurY, lineW, 1.0f);
            line.Color = Theme.BorderColor;
            line.Alpha = Theme.BorderAlpha;
            top.CurY += 1.0f + Theme.Spacing + 2.0f;
        }
        else if (o.Close) {
            top.CurY += 22.0f;
        }
        return open;
    }

    void UIContext::EndPanel() {
        while (!m_Stack.empty() && m_Stack.back().K != Box::Kind::Panel) CloseTop();
        if (m_Stack.empty()) throw std::runtime_error("ui.end_panel without ui.begin_panel");
        Box b = std::move(m_Stack.back());
        m_Stack.pop_back();
        if (b.AutoH) {
            const float content = std::max(0.0f, b.CurY - b.StartY - (b.CurY > b.StartY ? b.Spacing : 0.0f));
            const float h = content + 2.0f * b.Padding;
            m_PanelHeight[b.Id] = h;
            if (m_Out) for (size_t i : b.Patch) if (i < m_Out->size()) (*m_Out)[i].H = h / kUnitH;
            for (auto& [id, r] : m_Panels) if (id == b.Id) r.H = h;
        }
        if (b.AutoW) {
            const float w = std::max(0.0f, b.MaxRight - b.X) + 2.0f * b.Padding;
            m_PanelWidth[b.Id] = w;
            if (m_Out) for (size_t i : b.Patch) if (i < m_Out->size()) (*m_Out)[i].W = w / m_VW;
            for (auto& [id, r] : m_Panels) if (id == b.Id) r.W = w;
        }
        for (auto& [id, r] : m_Panels) if (id == b.Id) m_Last = r;   // ui.last_rect() after end_panel: the panel
        m_LastId.clear();
    }

    void UIContext::BeginColumn(float width) {
        if (m_Stack.empty()) throw std::runtime_error("ui.begin_column: call it inside a panel");
        const Box& parent = m_Stack.back();
        const bool inRow = parent.K == Box::Kind::Row;
        Box col;
        col.K = Box::Kind::Column;
        col.Id = parent.Id;
        col.X = inRow ? parent.CurX : parent.X;
        col.Y = col.CurY = col.StartY = inRow ? parent.Y : parent.CurY;
        col.W = width > 0.0f ? width : Avail();
        col.MaxRight = col.X;
        col.Spacing = Theme.Spacing;
        col.ItemStart = m_Out ? m_Out->size() : 0;
        m_Stack.push_back(std::move(col));
    }

    void UIContext::EndColumn() {
        if (m_Stack.empty() || m_Stack.back().K != Box::Kind::Column) throw std::runtime_error("ui.end_column without ui.begin_column");
        Box col = std::move(m_Stack.back());
        m_Stack.pop_back();
        const float h = std::max(0.0f, col.CurY - col.StartY - (col.CurY > col.StartY ? col.Spacing : 0.0f));
        ChildDone(col.X, col.Y, col.W, h, col.ItemStart);
    }

    void UIContext::BeginRow(float spacing) {
        if (m_Stack.empty()) throw std::runtime_error("ui.begin_row: call it inside a panel");
        const Box& parent = m_Stack.back();
        Box row;
        row.K = Box::Kind::Row;
        row.Id = parent.Id;
        row.X = parent.K == Box::Kind::Row ? parent.CurX : parent.X;
        row.Y = parent.K == Box::Kind::Row ? parent.Y : parent.CurY;
        row.W = Avail();
        row.CurX = row.X;
        row.MaxRight = row.X;
        row.Spacing = spacing >= 0.0f ? spacing : Theme.Spacing;
        row.ItemStart = m_Out ? m_Out->size() : 0;
        m_Stack.push_back(std::move(row));
    }

    void UIContext::EndRow() {
        if (m_Stack.empty() || m_Stack.back().K != Box::Kind::Row) throw std::runtime_error("ui.end_row without ui.begin_row");
        Box row = std::move(m_Stack.back());
        m_Stack.pop_back();
        // Centre each child vertically in the row
        if (m_Out) {
            for (size_t c = 0; c < row.Children.size(); c++) {
                const auto [start, h] = row.Children[c];
                const size_t end = c + 1 < row.Children.size() ? std::get<0>(row.Children[c + 1]) : m_Out->size();
                const float dy = (row.LineH - h) * 0.5f / kUnitH;
                if (dy > 0.0f) {
                    for (size_t i = start; i < end && i < m_Out->size(); i++) (*m_Out)[i].Y += dy;
                    for (NavItem& n : m_NavItems)    // the focusable widgets in it move down with it
                        if (n.OutIndex >= start && n.OutIndex < end) {
                            n.R.Y += dy * kUnitH;
                            n.ContentY += dy * kUnitH;
                            if (n.Id == m_NavId) m_NavRect = n.R;
                        }
                }
            }
        }
        const float used = std::max(0.0f, row.MaxRight - row.X);
        ChildDone(row.X, row.Y, used, row.LineH, row.ItemStart);
    }

    void UIContext::BeginScroll(const std::string& id, float height) {
        const std::string sid = MakeId(id);
        const float w = Avail();
        const Rect outer = Place(w, height);
        const float content = m_ScrollContent.count(sid) ? m_ScrollContent[sid] : 0.0f;
        const float maxOff = std::max(0.0f, content - height);
        float& off = m_ScrollOffset[sid];

        if (Hover(outer) && m_Wheel != 0.0f) off -= m_Wheel * 48.0f;
        // Dragging the thumb
        const float thumbH = content > height ? std::max(24.0f, height * height / content) : height;
        const Rect track{ outer.X + outer.W - 8.0f, outer.Y, 8.0f, height };
        const std::string bar = sid + "/bar";
        if (maxOff > 0.0f && Hover(track) && m_Pressed) { m_ActiveId = bar; m_PressY = m_PY; }
        if (m_ActiveId == bar && m_Down && maxOff > 0.0f)
            off = (m_PY - outer.Y - thumbH * 0.5f) / std::max(1.0f, height - thumbH) * maxOff;
        // Navigation moved the focus to a widget in here: scroll it into view
        if (!m_NavScrollTo.empty())
            if (const NavItem* t = PrevNavItem(m_NavScrollTo); t && t->ScrollId == sid) {
                const float margin = 6.0f;
                if (t->ContentY - margin < off) off = t->ContentY - margin;
                if (t->ContentY + t->R.H + margin > off + height) off = t->ContentY + t->R.H + margin - height;
                m_NavScrollTo.clear();
            }
        off = std::clamp(off, 0.0f, maxOff);

        const Rect clip = Clip();
        const float x0 = std::max(clip.X, outer.X), y0 = std::max(clip.Y, outer.Y);
        const float x1 = std::min(clip.X + clip.W, outer.X + outer.W), y1 = std::min(clip.Y + clip.H, outer.Y + outer.H);
        const Rect inner{ x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
        Emit(ScriptUIItem::Type::ClipPush, inner.X, inner.Y, inner.W, inner.H);
        m_Clips.push_back(inner);

        Box b;
        b.K = Box::Kind::Scroll;
        b.Id = CurrentPanel();
        b.ScrollId = sid;
        b.Outer = outer;
        b.X = outer.X;
        b.W = std::max(0.0f, w - 12.0f);       // room for the scroll bar
        b.Y = b.CurY = b.StartY = outer.Y - off;
        b.Spacing = Theme.Spacing;
        b.Scroll = off;
        m_Stack.push_back(std::move(b));
    }

    void UIContext::EndScroll() {
        if (m_Stack.empty() || m_Stack.back().K != Box::Kind::Scroll) throw std::runtime_error("ui.end_scroll without ui.begin_scroll");
        Box b = std::move(m_Stack.back());
        m_Stack.pop_back();
        const float content = std::max(0.0f, b.CurY - b.StartY - (b.CurY > b.StartY ? b.Spacing : 0.0f));
        m_ScrollContent[b.ScrollId] = content;
        Emit(ScriptUIItem::Type::ClipPop, 0, 0, 0, 0);
        if (!m_Clips.empty()) m_Clips.pop_back();

        const float h = b.Outer.H;
        if (content > h + 0.5f) {
            const float thumbH = std::max(24.0f, h * h / content);
            const float t = b.Scroll / (content - h);
            const float x = b.Outer.X + b.Outer.W - 6.0f;
            ScriptUIItem& track = Emit(ScriptUIItem::Type::Rect, x, b.Outer.Y, 5.0f, h);
            track.Color = glm::vec3(1.0f);
            track.Alpha = 0.06f;
            track.Rounding = 2.5f;
            ScriptUIItem& thumb = Emit(ScriptUIItem::Type::Rect, x, b.Outer.Y + (h - thumbH) * t, 5.0f, thumbH);
            thumb.Color = glm::vec3(1.0f);
            thumb.Alpha = m_ActiveId == b.ScrollId + "/bar" ? 0.6f : 0.32f;
            thumb.Rounding = 2.5f;
        }
        m_Last = b.Outer;
    }

    // ------------------------------------------------------------------ widgets
    void UIContext::Label(const std::string& text, const UITextOptions& o) {
        const float avail = o.Width > 0.0f ? o.Width : Avail();
        const float natural = LayoutText(text, 0, 0, 0, o, false).x;
        float w = (o.Align != 0 || (o.Wrap && natural > avail)) ? avail : natural;
        const float h = LayoutText(text, 0, 0, o.Wrap ? w : 0.0f, o, false).y;
        const Rect r = Place(w, h);
        LayoutText(text, r.X, r.Y, o.Wrap || o.Align != 0 ? w : 0.0f, o, true);
        m_Last = r;
        m_LastId.clear();
    }

    bool UIContext::Button(const std::string& label, const UIButtonOptions& o) {
        const float size = o.Size > 0.0f ? o.Size : Theme.ButtonTextSize;
        const float tw = TextWidth(label, size, true);
        const float w = o.Fill ? Avail() : (o.W > 0.0f ? o.W : std::max(tw + 2.0f * Theme.Padding, 64.0f));
        const float h = o.H > 0.0f ? o.H : Theme.ButtonHeight;
        const Rect r = Place(w, h);
        const std::string id = MakeId(o.Id.empty() ? label : o.Id);

        const bool nav = NavItemHere(id, r, NavKind::Button, o.DefaultFocus, o.Disabled);
        const bool hovered = !o.Disabled && !m_Dragging && Hover(r);
        if (hovered) SetHot(id);
        if (hovered && m_Pressed) m_ActiveId = id;
        const bool clicked = (hovered && m_Released && m_ActiveId == id) || (nav && m_NavActivate == id);
        const bool held = (hovered && m_Down && m_ActiveId == id) || (nav && !o.Disabled && m_NavIn.Confirm);

        glm::vec3 face = o.HasColor ? o.Color : (o.Selected ? Theme.AccentColor : Theme.ButtonColor);
        float alpha = 0.95f;
        if (o.Disabled) { face *= 0.7f; alpha = 0.5f; }
        else if (held) face *= 0.75f;
        else if (hovered || nav) face = glm::mix(face, glm::vec3(1.0f), 0.16f);
        ScriptUIItem& f = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
        f.Color = face;
        f.Alpha = alpha;
        f.Rounding = Theme.Rounding;
        ScriptUIItem& b = Emit(ScriptUIItem::Type::Border, r.X, r.Y, r.W, r.H);
        b.Color = o.Selected ? glm::mix(Theme.AccentColor, glm::vec3(1.0f), 0.4f) : glm::vec3(1.0f);
        b.Alpha = (hovered ? 0.5f : 0.2f) * alpha;
        b.Rounding = Theme.Rounding;
        b.Thickness = 1.5f;

        UITextOptions t;
        t.Size = size;
        t.Bold = true;
        t.Shadow = true;
        t.Align = 1;
        t.HasColor = true;
        t.Color = o.Disabled ? Theme.MutedColor : glm::vec3(1.0f);
        LayoutText(label, r.X, r.Y + (h - size * kLineSpacing) * 0.5f, r.W, t, true);
        if (nav) NavRing(r);

        m_Last = r;
        m_LastId = id;
        if (!o.Tooltip.empty()) Tooltip(o.Tooltip);
        return clicked && !o.Disabled;
    }

    void UIContext::Image(const std::string& path, float w, float h, const glm::vec3& color, float alpha) {
        if (w <= 0.0f) w = 64.0f;
        if (h <= 0.0f) h = w;
        const Rect r = Place(w, h);
        EmitImage(path, r.X, r.Y, r.W, r.H, color, alpha, 0.0f);
        m_Last = r;
        m_LastId.clear();
    }

    void UIContext::Bar(float fraction, float w, float h, const glm::vec3& color, const glm::vec3& back, const std::string& text) {
        if (w <= 0.0f) w = Avail();
        if (h <= 0.0f) h = 18.0f;
        const Rect r = Place(w, h);
        const float round = std::min(h * 0.5f, Theme.Rounding);
        ScriptUIItem& bg = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
        bg.Color = back;
        bg.Alpha = 0.75f;
        bg.Rounding = round;
        const float f = std::clamp(fraction, 0.0f, 1.0f);
        if (f > 0.0f) {
            ScriptUIItem& fill = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W * f, r.H);
            fill.Color = color;
            fill.Rounding = round;
        }
        if (!text.empty()) {
            UITextOptions t;
            t.Size = std::max(10.0f, h * 0.72f);
            t.Bold = true;
            t.Shadow = true;
            t.Align = 1;
            t.HasColor = true;
            t.Color = glm::vec3(1.0f);
            LayoutText(text, r.X, r.Y + (h - t.Size * kLineSpacing) * 0.5f, r.W, t, true);
        }
        m_Last = r;
        m_LastId.clear();
    }

    UISlotResult UIContext::Slot(const std::string& slotId, const UISlotOptions& o) {
        UISlotResult res;
        const float size = o.Size > 0.0f ? o.Size : Theme.SlotSize;
        const Rect r = Place(size, size);
        const std::string id = MakeId(slotId);

        const bool nav = NavItemHere(id, r, NavKind::Slot, o.DefaultFocus);
        const bool hovered = Hover(r);
        res.Hovered = hovered || nav;
        if (hovered) SetHot(id);
        // Navigation: Confirm picks a draggable slot's icon up, and puts it down on the next slot it confirms
        // (the same slot: put back); otherwise it clicks. Secondary right-clicks.
        if (nav && m_NavActivate == id) {
            if (m_NavCarry) {
                if (m_DragId != id && o.Accept) res.Dropped = true;
                m_Dragging = m_NavCarry = false;
                m_DragEnded = true;
                m_DragId.clear();
                m_DragIcon.clear();
            }
            else if (o.Draggable && !o.Icon.empty()) {
                m_Dragging = m_NavCarry = true;
                m_DragId = id;
                m_DragIcon = o.Icon;
                res.DragStarted = true;
            }
            else res.Clicked = true;
        }
        if (nav && m_NavSecondary == id && !m_NavCarry) res.RightClicked = true;
        if (hovered && m_Pressed) { m_ActiveId = id; m_PressX = m_PX; m_PressY = m_PY; }
        if (hovered && m_RightPressed) res.RightClicked = true;
        if (o.Draggable && !o.Icon.empty() && m_ActiveId == id && m_Down && !m_Dragging &&
            std::hypot(m_PX - m_PressX, m_PY - m_PressY) > kDragThreshold) {
            m_Dragging = true;
            m_DragId = id;
            m_DragIcon = o.Icon;
            res.DragStarted = true;
        }
        if (hovered && m_Released) {
            if (m_Dragging) res.Dropped = o.Accept && m_DragId != id;
            else if (m_ActiveId == id) res.Clicked = true;
        }

        const bool target = m_Dragging && (m_NavCarry ? nav : hovered) && o.Accept && m_DragId != id;
        const glm::vec3 bg = o.HasColor ? o.Color : Theme.SlotColor;
        if (!Theme.SlotImage.empty()) EmitImage(Theme.SlotImage, r.X, r.Y, r.W, r.H, glm::vec3(1.0f), 1.0f, Theme.SlotSlice);
        else {
            ScriptUIItem& f = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
            f.Color = hovered && !m_Dragging ? glm::mix(bg, glm::vec3(1.0f), 0.08f) : bg;
            f.Alpha = 0.95f;
            f.Rounding = 6.0f;
        }
        ScriptUIItem& b = Emit(ScriptUIItem::Type::Border, r.X, r.Y, r.W, r.H);
        b.Rounding = 6.0f;
        if (target)            { b.Color = Theme.AccentColor; b.Alpha = 1.0f; b.Thickness = 2.5f; }
        else if (o.Highlight)  { b.Color = Theme.TitleColor;  b.Alpha = 0.9f; b.Thickness = 2.0f; }
        else if (hovered || nav) { b.Color = glm::vec3(1.0f); b.Alpha = 0.45f; b.Thickness = 1.5f; }
        else                   { b.Color = Theme.BorderColor; b.Alpha = Theme.BorderAlpha + 0.06f; }

        if (!o.Icon.empty()) {
            const float inset = size * 0.1f;
            const float alpha = (m_Dragging && m_DragId == id) ? 0.3f : o.IconAlpha;
            EmitImage(o.Icon, r.X + inset, r.Y + inset, size - 2 * inset, size - 2 * inset, glm::vec3(1.0f), alpha, 0.0f);
        }
        if (o.Count > 1) {
            const float ts = std::max(11.0f, size * 0.27f);
            EmitText(std::to_string(o.Count), r.X + size - 5.0f, r.Y + size - ts * 1.15f, ts, glm::vec3(1.0f), 1.0f, true, true, 2);
        }
        if (nav) NavRing(r);
        m_Last = r;
        m_LastId = id;
        if (!o.Tooltip.empty()) Tooltip(o.Tooltip);
        return res;
    }

    int UIContext::Tabs(const std::string& id, const std::vector<std::string>& labels, int current) {
        BeginRow(4.0f);
        for (size_t i = 0; i < labels.size(); i++) {
            UIButtonOptions b;
            b.Id = id + "/" + std::to_string(i);
            b.Selected = (int)i == current;
            if (Button(labels[i], b)) current = (int)i;
        }
        EndRow();
        return current;
    }

    void UIContext::Space(float h) {
        if (!m_Stack.empty() && m_Stack.back().K == Box::Kind::Row) Place(h, 0.0f);
        else Place(0.0f, h);
        m_LastId.clear();
    }

    void UIContext::Separator() {
        if (!m_Stack.empty() && m_Stack.back().K == Box::Kind::Row) { Space(Theme.Spacing); return; }
        const Rect r = Place(Avail(), 1.0f);
        ScriptUIItem& line = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, 1.0f);
        line.Color = Theme.BorderColor;
        line.Alpha = Theme.BorderAlpha;
        m_Last = r;
        m_LastId.clear();
    }

    void UIContext::Tooltip(const std::string& text) {
        if (text.empty() || m_Dragging) return;
        bool hovered;
        if (m_LastId.empty()) {
            hovered = Hover(m_Last) && !m_HotFromNav;   // (the navigation focus's tooltip wins)
            if (hovered) m_HotId = CurrentPanel() + "/tip:" + text;
        }
        else hovered = m_LastId == m_HotId;
        if (hovered) m_PendingTooltip = text;
    }

    // ------------------------------------------------------------------ text field
    void UIContext::TakeFocus(const std::string& id, const std::string& text, bool selectAll) {
        m_FocusId = id;
        m_EditText = m_EditOriginal = m_EditReturned = text;
        m_Caret = text.size();
        m_Anchor = selectAll ? 0 : m_Caret;
        m_ScrollX = 0.0f;
        m_Blink = 0.0f;
        m_NavId = id;                            // navigation goes on from the field being typed in
    }

    UIInputResult UIContext::Input(const std::string& fieldId, const std::string& text, const UIInputOptions& o) {
        const float size = o.Size > 0.0f ? o.Size : Theme.TextSize;
        const float w = o.W > 0.0f ? o.W : Avail();
        const float h = o.H > 0.0f ? o.H : Theme.ButtonHeight;
        const Rect r = Place(w, h);
        const std::string id = MakeId(fieldId);
        if (!o.Disabled) m_InputOrder.push_back(id);
        const float padX = 8.0f;
        const Rect inner{ r.X + padX, r.Y, std::max(0.0f, r.W - 2.0f * padX), r.H };

        const bool nav = !o.Disabled && NavItemHere(id, r, NavKind::Input, o.DefaultFocus);
        const bool hovered = !o.Disabled && !m_Dragging && Hover(r);
        if (hovered) SetHot(id);
        UIInputResult res;

        // Taking the focus: Tab moved here, the script asked (Focus turned true), Confirm on it (navigation),
        // or a press on the field
        const bool ask = o.Focus && !m_PrevFocusAsks.count(id);
        if (o.Focus) m_FocusAsks.insert(id);
        bool focused = m_FocusId == id;
        bool byNav = false;
        if (o.Disabled && focused) { m_FocusId.clear(); focused = false; }
        if (!o.Disabled && !focused && (m_FocusRequest == id || ask || (nav && m_NavActivate == id))) {
            byNav = nav && m_NavActivate == id;   // (its Enter this frame was the Confirm: not a submit)
            TakeFocus(id, text, o.SelectAll);
            m_FocusRequest.clear();
            focused = true;
        }
        // The script changed the value under the field (cleared it after Enter, say): take its text
        if (focused && text != m_EditReturned) {
            m_EditText = m_EditReturned = text;
            m_Caret = m_Anchor = text.size();
        }

        std::string& s = m_EditText;
        auto display = [&](size_t upTo) {         // what is drawn for s[0, upTo)
            const std::string part = (focused ? s : text).substr(0, upTo);
            return o.Password ? std::string(CountCp(part), '*') : part;
        };
        auto caretAt = [&](float px) {            // the code point boundary nearest to px
            const std::string& t = focused ? s : text;
            size_t best = 0;
            float bestD = FLT_MAX;
            for (size_t i = 0;; i = NextCp(t, i)) {
                const float d = std::abs(inner.X - m_ScrollX + Measure(display(i), size, false) - px);
                if (d < bestD) { bestD = d; best = i; }
                if (i >= t.size()) break;
            }
            return best;
        };
        if (hovered && m_Pressed) {
            if (!focused) { TakeFocus(id, text, false); focused = true; }
            m_PressOnFocus = true;
            m_ActiveId = id;
            m_Caret = m_Anchor = caretAt(m_PX);
            m_Blink = 0.0f;
        }
        else if (focused && m_ActiveId == id && m_Down) {   // dragging selects
            m_Caret = caretAt(m_PX);
        }

        bool wasFocused = focused;
        if (focused) {
            m_FocusSeen = true;
            const size_t maxLen = o.MaxLength > 0 ? (size_t)o.MaxLength : (size_t)-1;
            auto selLo = [&]() { return std::min(m_Caret, m_Anchor); };
            auto selHi = [&]() { return std::max(m_Caret, m_Anchor); };
            auto deleteSel = [&]() {
                if (m_Caret == m_Anchor) return false;
                s.erase(selLo(), selHi() - selLo());
                m_Caret = m_Anchor = selLo();
                return true;
            };
            auto insert = [&](const std::string& in) {   // filtered, one line, up to MaxLength
                deleteSel();
                std::string add;
                size_t count = CountCp(s);
                for (size_t i = 0; i < in.size() && count < maxLen; i = NextCp(in, i)) {
                    const uint32_t cp = DecodeCp(in, i);
                    if (cp == '\n' || cp == '\r' || !FilterAllows(o.Filter, cp)) continue;
                    add += cp == '\t' ? std::string(" ") : in.substr(i, NextCp(in, i) - i);
                    count++;
                }
                s.insert(m_Caret, add);
                m_Caret = m_Anchor = m_Caret + add.size();
            };
            auto moveTo = [&](size_t to, bool extend) { m_Caret = to; if (!extend) m_Anchor = to; };

            for (const TextEvent& e : m_TextEvents) {
                if (m_FocusId != id) break;           // Enter / Escape / Tab let go: the rest isn't ours
                m_Blink = 0.0f;
                if (e.Char != 0) {
                    if (!(e.Mods & (Mod::Control | Mod::Super))) insert(EncodeCp(e.Char));
                    continue;
                }
                const bool shift = (e.Mods & Mod::Shift) != 0, ctrl = (e.Mods & Mod::Control) != 0;
                switch (e.Key) {
                case K::Backspace:
                    if (!deleteSel() && m_Caret > 0) {
                        const size_t from = ctrl ? WordLeft(s, m_Caret) : PrevCp(s, m_Caret);
                        s.erase(from, m_Caret - from);
                        m_Caret = m_Anchor = from;
                    }
                    break;
                case K::Delete:
                    if (!deleteSel() && m_Caret < s.size()) {
                        const size_t to = ctrl ? WordRight(s, m_Caret) : NextCp(s, m_Caret);
                        s.erase(m_Caret, to - m_Caret);
                    }
                    break;
                case K::Left:
                    if (!shift && m_Caret != m_Anchor && !ctrl) moveTo(selLo(), false);
                    else moveTo(ctrl ? WordLeft(s, m_Caret) : PrevCp(s, m_Caret), shift);
                    break;
                case K::Right:
                    if (!shift && m_Caret != m_Anchor && !ctrl) moveTo(selHi(), false);
                    else moveTo(ctrl ? WordRight(s, m_Caret) : NextCp(s, m_Caret), shift);
                    break;
                case K::Home: moveTo(0, shift); break;
                case K::End:  moveTo(s.size(), shift); break;
                case K::A: if (ctrl) { m_Anchor = 0; m_Caret = s.size(); } break;
                case K::C:
                case K::X:
                    if (ctrl && m_Caret != m_Anchor && !o.Password && ImGui::GetCurrentContext()) {
                        ImGui::SetClipboardText(s.substr(selLo(), selHi() - selLo()).c_str());
                        if (e.Key == K::X) deleteSel();
                    }
                    break;
                case K::V:
                    if (ctrl && ImGui::GetCurrentContext())
                        if (const char* clip = ImGui::GetClipboardText()) insert(clip);
                    break;
                case K::Enter:
                case K::KeypadEnter:
                    if (byNav) break;
                    res.Submitted = true;
                    if (!o.KeepFocus) m_FocusId.clear();
                    break;
                case K::Escape:
                    res.Cancelled = true;
                    s = m_EditOriginal;
                    m_FocusId.clear();
                    break;
                case K::Tab:
                    m_TabMove = shift ? -1 : 1;           // EndFrame moves the focus (the field order is known then)
                    break;
                default: break;
                }
            }
            focused = m_FocusId == id;
        }
        res.Text = wasFocused ? s : text;
        res.Changed = res.Text != text;
        res.Focused = focused;
        if (focused) m_EditReturned = res.Text;

        // Drawing: box, then (clipped to the inside) the selection, the text or hint, the caret
        const float alpha = o.Disabled ? 0.5f : 0.95f;
        ScriptUIItem& f = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
        f.Color = hovered && !focused ? glm::mix(Theme.SlotColor, glm::vec3(1.0f), 0.05f) : Theme.SlotColor;
        f.Alpha = alpha;
        f.Rounding = std::min(6.0f, Theme.Rounding);
        ScriptUIItem& b = Emit(ScriptUIItem::Type::Border, r.X, r.Y, r.W, r.H);
        b.Rounding = f.Rounding;
        if (focused)      { b.Color = Theme.AccentColor; b.Alpha = 1.0f; b.Thickness = 2.0f; }
        else if (hovered) { b.Color = glm::vec3(1.0f);   b.Alpha = 0.4f; b.Thickness = 1.5f; }
        else              { b.Color = Theme.BorderColor; b.Alpha = (Theme.BorderAlpha + 0.08f) * alpha; }

        const std::string& shown = focused ? s : res.Text;
        if (focused) {
            m_Caret = std::min(m_Caret, s.size());
            m_Anchor = std::min(m_Anchor, s.size());
        }
        const std::string full = o.Password ? std::string(CountCp(shown), '*') : shown;
        const float fullW = Measure(full, size, false);
        float scroll = 0.0f;
        float caretX = 0.0f;
        if (focused) {                            // keep the caret in view
            caretX = Measure(display(m_Caret), size, false);
            if (caretX - m_ScrollX > inner.W - 2.0f) m_ScrollX = caretX - inner.W + 2.0f;
            if (caretX - m_ScrollX < 0.0f) m_ScrollX = caretX;
            m_ScrollX = std::clamp(m_ScrollX, 0.0f, std::max(0.0f, fullW - inner.W + 2.0f));
            scroll = m_ScrollX;
        }
        const Rect clip = Clip();
        const float x0 = std::max(clip.X, inner.X), y0 = std::max(clip.Y, inner.Y);
        const float x1 = std::min(clip.X + clip.W, inner.X + inner.W), y1 = std::min(clip.Y + clip.H, inner.Y + inner.H);
        const Rect in{ x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
        Emit(ScriptUIItem::Type::ClipPush, in.X, in.Y, in.W, in.H);
        m_Clips.push_back(in);

        const float lineH = size * 1.2f;
        const float textY = r.Y + (h - size) * 0.5f;
        if (focused && m_Caret != m_Anchor) {
            const float a = Measure(display(std::min(m_Caret, m_Anchor)), size, false);
            const float c = Measure(display(std::max(m_Caret, m_Anchor)), size, false);
            ScriptUIItem& sel = Emit(ScriptUIItem::Type::Rect, inner.X - scroll + a, r.Y + (h - lineH) * 0.5f, c - a, lineH);
            sel.Color = Theme.AccentColor;
            sel.Alpha = 0.55f;
        }
        if (!full.empty())
            EmitText(full, inner.X - scroll, textY, size, Theme.TextColor, o.Disabled ? 0.6f : 1.0f, false, false);
        else if (!o.Hint.empty())
            EmitText(o.Hint, inner.X, textY, size, Theme.MutedColor, 0.8f, false, false);
        if (focused && std::fmod(m_Blink, 1.06f) < 0.53f) {
            ScriptUIItem& caret = Emit(ScriptUIItem::Type::Rect, inner.X - scroll + caretX, r.Y + (h - lineH) * 0.5f, 1.5f, lineH);
            caret.Color = Theme.TextColor;
        }
        Emit(ScriptUIItem::Type::ClipPop, 0, 0, 0, 0);
        m_Clips.pop_back();
        if (nav && !focused) NavRing(r);

        m_Last = r;
        m_LastId = id;
        if (!o.Tooltip.empty()) Tooltip(o.Tooltip);
        return res;
    }

    // ------------------------------------------------------------------ slider
    UISliderResult UIContext::Slider(const std::string& sliderId, float value, const UISliderOptions& o) {
        const float w = o.W > 0.0f ? o.W : Avail();
        const float h = o.H > 0.0f ? o.H : 24.0f;
        const Rect r = Place(w, h);
        const std::string id = MakeId(sliderId);
        const bool nav = !o.Disabled && NavItemHere(id, r, NavKind::Slider, o.DefaultFocus);
        const bool hovered = !o.Disabled && !m_Dragging && Hover(r);
        if (hovered) SetHot(id);

        const float lo = std::min(o.Min, o.Max), hi = std::max(o.Max, o.Min + 1e-6f);
        const float knobW = std::min(12.0f, r.W * 0.2f);
        float v = std::clamp(value, lo, hi);
        if (hovered && m_Pressed) m_ActiveId = id;
        if (!o.Disabled && m_ActiveId == id && m_Down)
            v = lo + std::clamp((m_PX - r.X - knobW * 0.5f) / std::max(1.0f, r.W - knobW), 0.0f, 1.0f) * (hi - lo);
        if (nav && m_NavAdjust != 0) v = std::clamp(v + (o.Step > 0.0f ? o.Step : (hi - lo) / 20.0f) * (float)m_NavAdjust, lo, hi);
        if (o.Step > 0.0f) v = std::clamp(lo + std::round((v - lo) / o.Step) * o.Step, lo, hi);
        UISliderResult res;
        res.Value = v;
        res.Changed = v != value;

        const float t = (v - lo) / (hi - lo);
        const float alpha = o.Disabled ? 0.5f : 0.95f;
        const float round = std::min(h * 0.5f, Theme.Rounding);
        const bool lit = hovered || nav || m_ActiveId == id;
        ScriptUIItem& bg = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, r.W, r.H);
        bg.Color = Theme.SlotColor;
        bg.Alpha = alpha;
        bg.Rounding = round;
        const glm::vec3 fillColor = o.HasColor ? o.Color : Theme.AccentColor;
        const float fillW = knobW * 0.5f + (r.W - knobW) * t;
        if (fillW > 0.5f) {
            ScriptUIItem& fill = Emit(ScriptUIItem::Type::Rect, r.X, r.Y, fillW, r.H);
            fill.Color = lit ? glm::mix(fillColor, glm::vec3(1.0f), 0.15f) : fillColor;
            fill.Alpha = alpha;
            fill.Rounding = round;
        }
        ScriptUIItem& b = Emit(ScriptUIItem::Type::Border, r.X, r.Y, r.W, r.H);
        b.Color = lit ? glm::vec3(1.0f) : Theme.BorderColor;
        b.Alpha = lit ? 0.45f : (Theme.BorderAlpha + 0.08f) * alpha;
        b.Rounding = round;
        ScriptUIItem& knob = Emit(ScriptUIItem::Type::Rect, r.X + (r.W - knobW) * t, r.Y - 2.0f, knobW, r.H + 4.0f);
        knob.Color = o.Disabled ? Theme.MutedColor : glm::vec3(0.95f);
        knob.Alpha = alpha;
        knob.Rounding = std::min(4.0f, knobW * 0.5f);
        if (!o.Text.empty()) {
            UITextOptions to;
            to.Size = std::max(10.0f, std::min(h * 0.72f, Theme.TextSize));
            to.Bold = true;
            to.Shadow = true;
            to.Align = 1;
            to.HasColor = true;
            to.Color = o.Disabled ? Theme.MutedColor : glm::vec3(1.0f);
            LayoutText(o.Text, r.X, r.Y + (h - to.Size * kLineSpacing) * 0.5f, r.W, to, true);
        }
        if (nav) NavRing(r);
        m_Last = r;
        m_LastId = id;
        if (!o.Tooltip.empty()) Tooltip(o.Tooltip);
        return res;
    }

    // ------------------------------------------------------------------ navigation
    UINavInput UINavFromDevices(bool withBack) {
        UINavInput n;
        n.Up = Input::IsKeyDown(K::Up);
        n.Down = Input::IsKeyDown(K::Down);
        n.Left = Input::IsKeyDown(K::Left);
        n.Right = Input::IsKeyDown(K::Right);
        n.Confirm = Input::IsKeyDown(K::Enter) || Input::IsKeyDown(K::KeypadEnter);
        n.Back = withBack && Input::IsKeyDown(K::Escape);
        const GamepadState& p = Input::Gamepad();
        if (p.Connected) {
            n.Up = n.Up || p.Buttons[Pad::DPadUp] || p.Axes[Pad::LeftY] > 0.5f;
            n.Down = n.Down || p.Buttons[Pad::DPadDown] || p.Axes[Pad::LeftY] < -0.5f;
            n.Left = n.Left || p.Buttons[Pad::DPadLeft] || p.Axes[Pad::LeftX] < -0.5f;
            n.Right = n.Right || p.Buttons[Pad::DPadRight] || p.Axes[Pad::LeftX] > 0.5f;
            n.Confirm = n.Confirm || p.Buttons[Pad::A];
            n.Back = n.Back || (withBack && p.Buttons[Pad::B]);
            n.Secondary = p.Buttons[Pad::X];
        }
        return n;
    }

    bool UIContext::NavItemHere(const std::string& id, const Rect& r, NavKind kind, bool isDefault, bool disabled) {
        NavItem it;
        it.Id = id;
        it.R = r;
        it.Kind = kind;
        it.Default = isDefault;
        it.Disabled = disabled;
        it.OutIndex = m_Out ? m_Out->size() : 0;
        const std::string& panel = CurrentPanel();
        for (int i = (int)m_Panels.size() - 1; i >= 0; i--) if (m_Panels[(size_t)i].first == panel) { it.Panel = i; break; }
        for (auto b = m_Stack.rbegin(); b != m_Stack.rend(); ++b)
            if (b->K == Box::Kind::Scroll) { it.ScrollId = b->ScrollId; it.ContentY = r.Y - b->StartY; break; }
        m_NavItems.push_back(std::move(it));
        if (m_NavId != id) return false;
        m_NavRect = r;
        if (!m_NavVisible) return false;
        m_HotId = id;                            // its tooltip shows (by the widget)
        m_HotFromNav = true;
        return true;
    }

    void UIContext::NavRing(const Rect& r) {
        const float g = 3.0f;
        ScriptUIItem& ring = Emit(ScriptUIItem::Type::Border, r.X - g, r.Y - g, r.W + 2 * g, r.H + 2 * g);
        ring.Color = glm::mix(Theme.AccentColor, glm::vec3(1.0f), 0.45f);
        ring.Alpha = 1.0f;
        ring.Thickness = 2.5f;
        ring.Rounding = Theme.Rounding + g;
    }

    const UIContext::NavItem* UIContext::PrevNavItem(const std::string& id) const {
        if (id.empty()) return nullptr;
        for (const NavItem& n : m_PrevNavItems) if (n.Id == id) return &n;
        return nullptr;
    }

    std::string UIContext::NavDefaultId() const {
        // The panel that just appeared, else the topmost one (drawn last) with something enabled to focus
        int panel = -1;
        if (!m_NavPreferPanel.empty())
            for (const NavItem& n : m_PrevNavItems)
                if (!n.Disabled && n.Panel >= 0 && (size_t)n.Panel < m_PrevPanels.size() && m_PrevPanels[(size_t)n.Panel].first == m_NavPreferPanel)
                    panel = n.Panel;
        if (panel < 0) for (const NavItem& n : m_PrevNavItems) if (!n.Disabled) panel = std::max(panel, n.Panel);
        const NavItem* first = nullptr;
        for (const NavItem& n : m_PrevNavItems) {
            if (n.Panel != panel || n.Disabled) continue;
            if (n.Default) return n.Id;
            if (!first) first = &n;
        }
        return first ? first->Id : std::string();
    }

    // The nearest widget in a direction: its centre must lie past the current one's edge that way; distance along the
    // direction counts once, sideways distance (the gap between the two spans) three times plus a penalty, the
    // offset between centres half, and the gap may not be
    // more than the distance along (a cone of about 45 degrees). Its own panel first.
    std::string UIContext::NavMove(int dx, int dy) const {
        const NavItem* cur = PrevNavItem(m_NavId);
        if (!cur) return std::string();
        const float cx = cur->R.X + cur->R.W * 0.5f, cy = cur->R.Y + cur->R.H * 0.5f;
        const NavItem* best[2] = { nullptr, nullptr };     // same panel, any panel
        float bestScore[2] = { FLT_MAX, FLT_MAX };
        for (const NavItem& n : m_PrevNavItems) {
            if (&n == cur) continue;
            const float nx = n.R.X + n.R.W * 0.5f, ny = n.R.Y + n.R.H * 0.5f;
            float along, gap, off, beyond;
            if (dy != 0) {
                along = (ny - cy) * (float)dy;
                beyond = (dy > 0 ? ny - (cur->R.Y + cur->R.H) : cur->R.Y - ny);   // its centre past my edge
                gap = std::max(0.0f, std::max(n.R.X - (cur->R.X + cur->R.W), cur->R.X - (n.R.X + n.R.W)));
                off = std::abs(nx - cx);
            }
            else {
                along = (nx - cx) * (float)dx;
                beyond = (dx > 0 ? nx - (cur->R.X + cur->R.W) : cur->R.X - nx);
                gap = std::max(0.0f, std::max(n.R.Y - (cur->R.Y + cur->R.H), cur->R.Y - (n.R.Y + n.R.H)));
                off = std::abs(ny - cy);
            }
            if (beyond < 0.0f || along < 1.0f || gap > along) continue;   // that way, within about 45 degrees
            // Widgets level with it (spans overlapping) are strongly preferred to ones off to a side
            const float score = along + gap * 3.0f + off * 0.5f + (gap > 0.0f ? 100.0f : 0.0f);
            for (int k = 0; k < 2; k++) {
                if (k == 0 && n.Panel != cur->Panel) continue;
                if (score < bestScore[k]) { bestScore[k] = score; best[k] = &n; }
            }
        }
        const NavItem* to = best[0] ? best[0] : best[1];
        return to ? to->Id : std::string();
    }

    bool UIContext::ConsumeBack() {
        m_BackAsked = true;
        if (!m_BackPressed || m_BackUsed) return false;
        m_BackUsed = true;
        return true;
    }

    void UIContext::NavBegin() {
        const UINavInput& n = m_NavIn;
        m_NavActivate.clear();
        m_NavSecondary.clear();
        m_NavAdjust = 0;
        m_BackUsed = false;
        m_BackPanel.clear();
        m_NavSeen = false;
        m_NavRect = Rect{};
        const std::vector<std::string> closable = std::move(m_ClosablePanels);   // last frame's
        m_ClosablePanels.clear();
        m_WantsBack = !closable.empty() || m_BackAsked;
        m_BackAsked = false;

        // Directions: a press, then repeats while held (0.4 s, then every 0.11 s)
        const bool held[4] = { n.Up, n.Down, n.Left, n.Right };
        const bool was[4] = { m_NavPrev.Up, m_NavPrev.Down, m_NavPrev.Left, m_NavPrev.Right };
        bool press[4] = {};
        for (int i = 0; i < 4; i++) {
            if (held[i] && !was[i]) { press[i] = true; m_NavRepeat[i] = 0.4f; }
            else if (held[i]) {
                m_NavRepeat[i] -= m_Dt;
                if (m_NavRepeat[i] <= 0.0f) { press[i] = true; m_NavRepeat[i] = 0.11f; }
            }
        }
        bool confirm = n.Confirm && !m_NavPrev.Confirm;
        bool back = n.Back && !m_NavPrev.Back;
        bool secondary = n.Secondary && !m_NavPrev.Secondary;
        int tab = 0;
        if (m_FocusId.empty())
            for (const TextEvent& e : m_TextEvents) if (e.Char == 0 && e.Key == K::Tab) tab = (e.Mods & Mod::Shift) ? -1 : 1;
        if (n.Gamepad && !m_NavPrev.Gamepad) m_NavVisible = true;    // switched to the pad: show where the focus is
        m_NavPrev = n;

        // The pointer moved or pressed: the mouse is in charge again (the ring hides; the focus follows hover)
        m_PtrMoved = m_Inside && m_PrevPX > -1e5f && (std::abs(m_PX - m_PrevPX) > 2.0f || std::abs(m_PY - m_PrevPY) > 2.0f);
        if (m_Inside) { m_PrevPX = m_PX; m_PrevPY = m_PY; }
        if (m_PtrMoved || m_Pressed || m_RightPressed) m_NavVisible = false;

        // Typing in a field: the keyboard's keys are the field's (and masked from the actions), so these come
        // from the pad: Back cancels the typing, Confirm submits it, a direction leaves the field and moves
        if (!m_FocusId.empty()) {
            if (back) { m_TextEvents.push_back(TextEvent{ 0, K::Escape, 0 }); back = false; }
            if (confirm) { m_TextEvents.push_back(TextEvent{ 0, K::Enter, 0 }); confirm = false; }
            if (press[0] || press[1] || press[2] || press[3]) m_FocusId.clear();
            secondary = false;
        }
        m_BackPressed = back;

        const bool any = press[0] || press[1] || press[2] || press[3] || confirm || secondary || tab != 0;
        if (any) {
            const NavItem* cur = PrevNavItem(m_NavId);
            if (!m_NavVisible || !cur) {
                // The first press only shows the focus (on what it was, or the default)
                m_NavVisible = true;
                if (!cur) { m_NavId = NavDefaultId(); m_NavScrollTo = m_NavId; }
            }
            else {
                int dx = (press[3] ? 1 : 0) - (press[2] ? 1 : 0), dy = (press[1] ? 1 : 0) - (press[0] ? 1 : 0);
                if (cur->Kind == NavKind::Slider && dx != 0) { m_NavAdjust = dx; dx = 0; }
                if (dx != 0 && dy != 0) dx = 0;  // one axis at a time
                if (dx != 0 || dy != 0) {
                    const std::string to = NavMove(dx, dy);
                    if (!to.empty()) { m_NavId = to; m_NavScrollTo = to; }
                }
                if (tab != 0 && !m_PrevNavItems.empty()) {
                    const int count = (int)m_PrevNavItems.size();
                    int i = (int)(cur - m_PrevNavItems.data());
                    for (int k = 0; k < count; k++) {    // skip disabled ones
                        i = ((i + tab) % count + count) % count;
                        if (!m_PrevNavItems[(size_t)i].Disabled) break;
                    }
                    m_NavId = m_PrevNavItems[(size_t)i].Id;
                    m_NavScrollTo = m_NavId;
                }
                if (confirm) m_NavActivate = m_NavId;
                if (secondary) m_NavSecondary = m_NavId;
            }
        }
        if (m_NavPickDefault) {
            m_NavPickDefault = false;
            const std::string d = NavDefaultId();
            if (!d.empty()) { m_NavId = d; m_NavScrollTo = d; }
        }
        m_NavPreferPanel.clear();

        // Back: puts a carried icon back; else closes the focused widget's panel if it can close, else the
        // topmost panel that can; else ui.back() gets it
        if (back) {
            if (m_NavCarry) {
                m_Dragging = m_NavCarry = false;
                m_DragEnded = true;
                m_DragId.clear();
                m_DragIcon.clear();
                m_BackUsed = true;
            }
            else {
                auto canClose = [&](const std::string& p) { return std::find(closable.begin(), closable.end(), p) != closable.end(); };
                const NavItem* cur = PrevNavItem(m_NavId);
                if (cur && cur->Panel >= 0 && (size_t)cur->Panel < m_PrevPanels.size() && canClose(m_PrevPanels[(size_t)cur->Panel].first))
                    m_BackPanel = m_PrevPanels[(size_t)cur->Panel].first;
                else
                    for (auto it = m_PrevPanels.rbegin(); it != m_PrevPanels.rend(); ++it)
                        if (canClose(it->first)) { m_BackPanel = it->first; break; }
            }
        }
    }

    void UIContext::NavEnd() {
        // The pointer moved onto a focusable widget: navigation goes on from there
        if (m_PtrMoved && !m_HotFromNav && !m_HotId.empty())
            for (const NavItem& n : m_NavItems) if (n.Id == m_HotId) { m_NavId = n.Id; break; }
        // A panel that wasn't there last frame and has something to focus takes the focus
        for (const auto& [id, r] : m_Panels) {
            bool before = false;
            for (const auto& p : m_PrevPanels) if (p.first == id) { before = true; break; }
            if (before) continue;
            for (const NavItem& n : m_NavItems)
                if (!n.Disabled && n.Panel >= 0 && (size_t)n.Panel < m_Panels.size() && m_Panels[(size_t)n.Panel].first == id) {
                    m_NavPreferPanel = id;
                    m_NavPickDefault = true;
                    break;
                }
        }
        // The focused widget wasn't drawn: the default takes over (next frame, from this frame's widgets)
        if (!m_NavId.empty()) {
            bool seen = false;
            for (const NavItem& n : m_NavItems) if (n.Id == m_NavId) { seen = true; break; }
            if (!seen) { m_NavId.clear(); m_NavPickDefault = true; }
        }
        if (m_NavCarry && !m_Dragging) m_NavCarry = false;
        m_PrevNavItems = std::move(m_NavItems);
        m_NavItems.clear();
    }

    // ------------------------------------------------------------------ queries
    void UIContext::LastRect(float& x, float& y, float& w, float& h) const {
        x = m_Last.X / m_VW; y = m_Last.Y / kUnitH; w = m_Last.W / m_VW; h = m_Last.H / kUnitH;
    }

    bool UIContext::WantsMouse() const {
        if (m_Dragging) return true;
        if (!m_Inside) return false;
        for (const auto* list : { &m_PrevPanels, &m_Panels })
            for (const auto& [id, r] : *list) if (r.Contains(m_PX, m_PY)) return true;
        for (const auto* list : { &m_PrevBlockers, &m_Blockers })
            for (const Rect& r : *list) if (r.Contains(m_PX, m_PY)) return true;
        return false;
    }

    void UIContext::AddBlocker(float x, float y, float w, float h) {
        m_Blockers.push_back(Rect{ x * m_VW, y * kUnitH, w * m_VW, h * kUnitH });
    }

} // namespace Engine
