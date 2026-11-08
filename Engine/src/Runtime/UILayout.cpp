#include "Runtime/UILayout.h"

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
            Rect r{ m_PX + 18.0f, m_PY + 22.0f, textW + 2 * pad, m.y + 2 * pad };
            if (r.X + r.W > m_VW - 4) r.X = m_PX - r.W - 8;
            if (r.Y + r.H > kUnitH - 4) r.Y = m_PY - r.H - 8;
            EmitFrame(r, Theme.PanelColor * 0.8f, 0.97f, std::string(), 0.0f, 6.0f);
            LayoutText(m_PendingTooltip, r.X + pad, r.Y + pad, textW, o, true);
        }
        // The dragged icon follows the pointer
        if (m_Dragging && !m_DragIcon.empty() && m_Out)
            EmitImage(m_DragIcon, m_PX - 28.0f, m_PY - 28.0f, 56.0f, 56.0f, glm::vec3(1.0f), 0.9f, 0.0f);

        if (m_Released) {
            if (m_Dragging) m_DragEnded = true;
            m_Dragging = false;
            m_DragId.clear();
            m_DragIcon.clear();
            m_ActiveId.clear();
        }
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
                if (dy > 0.0f) for (size_t i = start; i < end && i < m_Out->size(); i++) (*m_Out)[i].Y += dy;
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

        const bool hovered = !o.Disabled && !m_Dragging && Hover(r);
        if (hovered) m_HotId = id;
        if (hovered && m_Pressed) m_ActiveId = id;
        const bool clicked = hovered && m_Released && m_ActiveId == id;
        const bool held = hovered && m_Down && m_ActiveId == id;

        glm::vec3 face = o.HasColor ? o.Color : (o.Selected ? Theme.AccentColor : Theme.ButtonColor);
        float alpha = 0.95f;
        if (o.Disabled) { face *= 0.7f; alpha = 0.5f; }
        else if (held) face *= 0.75f;
        else if (hovered) face = glm::mix(face, glm::vec3(1.0f), 0.16f);
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

        const bool hovered = Hover(r);
        res.Hovered = hovered;
        if (hovered) m_HotId = id;
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

        const bool target = m_Dragging && hovered && o.Accept && m_DragId != id;
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
        else if (hovered)      { b.Color = glm::vec3(1.0f);   b.Alpha = 0.45f; b.Thickness = 1.5f; }
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
            hovered = Hover(m_Last);
            if (hovered) m_HotId = CurrentPanel() + "/tip:" + text;
        }
        else hovered = m_LastId == m_HotId;
        if (hovered) m_PendingTooltip = text;
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
