#pragma once
#include "Scripting/ScriptEngine.h"     // ScriptUIItem

#include <glm/glm.hpp>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ImFont;

namespace Engine {

    // How laid-out UI looks. Sizes are in "units": pixels at a 720 px tall view (they scale with it).
    struct UITheme {
        glm::vec3 PanelColor{ 0.055f, 0.065f, 0.09f };  float PanelAlpha = 0.9f;
        std::string PanelImage;                         float PanelSlice = 0.0f;   // a 9-slice frame instead of a flat panel
        glm::vec3 BorderColor{ 1.0f };                  float BorderAlpha = 0.14f;
        glm::vec3 TitleColor{ 1.0f, 0.85f, 0.4f };      float TitleSize = 24.0f;
        glm::vec3 TextColor{ 0.93f, 0.94f, 0.96f };     float TextSize = 18.0f;
        glm::vec3 MutedColor{ 0.62f, 0.67f, 0.75f };
        glm::vec3 ButtonColor{ 0.16f, 0.2f, 0.3f };     glm::vec3 AccentColor{ 0.25f, 0.45f, 0.85f };
        float ButtonTextSize = 19.0f, ButtonHeight = 36.0f;
        float Padding = 14.0f, Spacing = 8.0f, Rounding = 8.0f;
        glm::vec3 SlotColor{ 0.1f, 0.12f, 0.16f };      float SlotSize = 64.0f;
        std::string SlotImage;                          float SlotSlice = 0.0f;
        float TooltipWidth = 320.0f;
    };

    // The pointer over the view (0..1, top left origin) and its buttons this frame
    struct UIPointerState {
        float X = 0.0f, Y = 0.0f;
        bool Inside = false, Down = false, RightDown = false;
        float Wheel = 0.0f;                      // notches this frame, + = away from the user (scroll up)
    };

    struct UITextOptions {
        float Size = 0.0f;                       // 0: the theme's text size
        bool HasColor = false; glm::vec3 Color{ 1.0f };
        float Alpha = 1.0f;
        int Align = 0;                           // 0 left, 1 centre, 2 right (within the width)
        bool Bold = false, Wrap = true, Shadow = false, Muted = false;
        float Width = 0.0f;                      // 0: what the container has left
    };

    struct UIPanelOptions {
        float X = 0.5f, Y = 0.5f;                // anchor point, 0..1 of the view
        glm::vec2 Pivot{ 0.5f };                 // which point of the panel sits there (0,0 top left, 1,1 bottom right)
        float W = 400.0f, H = 0.0f;              // units; 0 = as wide / tall as the content (an auto-width panel doesn't wrap text)
        std::string Title;
        bool Close = false;                      // an x in the corner
        float Padding = -1.0f;                   // < 0: the theme's
        bool HasColor = false; glm::vec3 Color{ 0.0f };
        float Alpha = -1.0f;
        std::string Image; float Slice = -1.0f;
    };

    struct UIButtonOptions {
        std::string Id;                          // default: the label (repeats in one panel are told apart by order)
        float W = 0.0f, H = 0.0f;                // 0: fit the label / the theme's height
        bool Fill = false;                       // as wide as the container has left
        float Size = 0.0f;
        bool HasColor = false; glm::vec3 Color{ 0.0f };
        bool Disabled = false, Selected = false;
        std::string Tooltip;
    };

    struct UISlotOptions {
        std::string Icon;                        // image path, empty = an empty slot
        int Count = 0;                           // shown in the corner when > 1
        float Size = 0.0f;
        std::string Tooltip;
        bool Draggable = false;                  // the icon can be dragged to another slot
        bool Accept = true;                      // things can be dropped here
        bool Highlight = false;
        bool HasColor = false; glm::vec3 Color{ 0.0f };
        float IconAlpha = 1.0f;
    };

    struct UISlotResult {
        bool Clicked = false, RightClicked = false, Hovered = false;
        bool Dropped = false;                    // a drag from another slot ended here this frame
        bool DragStarted = false;                // a drag from this slot began this frame
    };

    // Immediate-mode laid-out UI (ui.begin_panel, ui.label, ui.slot ... in Lua). Every frame the script
    // calls the widgets again; this places them (panels, rows, scroll regions), answers hover / click /
    // drag questions from the pointer, and writes ScriptUIItems, which the app draws over the view.
    // Positions inside panels are in units (see UITheme); items go out in 0..1 of the view.
    // Hover goes only to the topmost panel under the pointer (as of the last frame, since later panels
    // are drawn on top). Buttons click on release. Tooltips appear after a short hover.
    class UIContext {
    public:
        UITheme Theme;

        void SetFonts(ImFont* bold, ImFont* regular) { m_Bold = bold; m_Regular = regular; }
        void Reset();                            // forget panels, scroll, drags (a new game)

        void BeginFrame(std::vector<ScriptUIItem>& out, float aspect, const UIPointerState& pointer, float dt);
        // Tooltips and the dragged icon go on top; containers left open are closed (returns their count)
        int EndFrame();

        // ---- containers (each Begin needs its End)
        bool BeginPanel(const std::string& id, const UIPanelOptions& o);   // returns false when its x was clicked
        void EndPanel();
        void BeginRow(float spacing = -1.0f);    // children side by side, centred vertically
        void EndRow();
        void BeginScroll(const std::string& id, float height);   // wheel / bar scrolling, contents clipped
        void BeginColumn(float width);           // children stacked, `width` wide (e.g. side by side in a row)
        void EndColumn();
        void EndScroll();
        bool InContainer() const { return !m_Stack.empty(); }

        // ---- widgets (inside a container)
        void Label(const std::string& text, const UITextOptions& o);
        bool Button(const std::string& label, const UIButtonOptions& o);
        void Image(const std::string& path, float w, float h, const glm::vec3& color, float alpha);
        void Bar(float fraction, float w, float h, const glm::vec3& color, const glm::vec3& back, const std::string& text);
        UISlotResult Slot(const std::string& id, const UISlotOptions& o);
        int Tabs(const std::string& id, const std::vector<std::string>& labels, int current);
        void Space(float h);
        void Separator();
        void Tooltip(const std::string& text);   // for the widget just placed, when it is hovered

        // ---- queries
        void LastRect(float& x, float& y, float& w, float& h) const;   // the widget just placed, 0..1 of the view
        bool WantsMouse() const;                 // the pointer is over UI (or dragging): the game should ignore it
        bool Dragging() const { return m_Dragging; }
        bool DragEndedLastFrame() const { return m_DragEndedLast; }
        void AddBlocker(float x, float y, float w, float h);   // 0..1 of the view: a ui.button outside the layout
        float TextWidth(const std::string& text, float size, bool bold);   // units, one line, markup removed

    private:
        struct Rect {
            float X = 0, Y = 0, W = 0, H = 0;
            bool Contains(float px, float py) const { return px >= X && px <= X + W && py >= Y && py <= Y + H; }
        };
        struct Box {
            enum class Kind { Panel, Row, Scroll, Column } K = Kind::Panel;
            std::string Id;
            float X = 0, Y = 0, W = 0;           // content area (top left, width)
            float CurX = 0, CurY = 0;            // where the next child goes
            float LineH = 0;                     // rows: the tallest child so far
            float Spacing = 8;
            float StartY = 0;                    // panels / scroll: where the content began
            Rect Outer;                          // the panel or scroll region on screen
            std::vector<size_t> Patch;           // panels: background items whose height is set at End
            std::vector<std::tuple<size_t, float>> Children;   // rows: (first item, height) per child, centred at End
            size_t ClipItem = 0;
            float Padding = 0;
            float Scroll = 0;
            std::string ScrollId;                // scroll regions: the key of their offset
            size_t ItemStart = 0;                // rows: the first item of the row
            bool AutoH = false;                  // panels: as tall as their content
            bool AutoW = false;                  // panels: as wide as their content
            float MaxRight = 0;                  // the right edge of the widest child (auto width)
        };

        // Emitting items (units in, 0..1 of the view out)
        ScriptUIItem& Emit(ScriptUIItem::Type type, float x, float y, float w, float h);
        void EmitText(const std::string& text, float x, float y, float size, const glm::vec3& color, float alpha, bool bold, bool shadow, int align = 0);
        void EmitImage(const std::string& path, float x, float y, float w, float h, const glm::vec3& color, float alpha, float slice);
        void EmitFrame(const Rect& r, const glm::vec3& color, float alpha, const std::string& image, float slice, float rounding);

        Rect Place(float w, float h);            // the next slot in the current container
        float Avail() const;                     // width the current container has left
        bool Hover(const Rect& r) const;
        std::string MakeId(const std::string& base);
        const std::string& CurrentPanel() const;
        Rect Clip() const;
        ImFont* FontFor(bool bold);
        float Measure(const std::string& text, float size, bool bold);
        glm::vec2 LayoutText(const std::string& text, float x, float y, float width, const UITextOptions& o, bool emit);

        ImFont* m_Bold = nullptr;
        ImFont* m_Regular = nullptr;
        std::vector<ScriptUIItem>* m_Out = nullptr;
        float m_VW = 1280.0f;                    // view width in units (the height is 720)
        float m_Dt = 0.0f;

        // pointer, in units
        float m_PX = 0, m_PY = 0, m_Wheel = 0;
        bool m_Inside = false, m_Down = false, m_PrevDown = false, m_Pressed = false, m_Released = false;
        bool m_RightDown = false, m_PrevRightDown = false, m_RightPressed = false;

        std::vector<Box> m_Stack;
        std::vector<Rect> m_Clips;
        Rect m_Last;
        std::string m_LastId;
        std::unordered_map<std::string, int> m_IdCount;          // this frame: repeats of an id

        // panels: last frame's (in draw order) and this frame's; their measured heights
        std::vector<std::pair<std::string, Rect>> m_Panels, m_PrevPanels;
        std::vector<Rect> m_Blockers, m_PrevBlockers;
        std::unordered_map<std::string, float> m_PanelHeight, m_PanelWidth;
        void CloseTop();                         // end whatever container is innermost
        void ChildDone(float x, float y, float w, float h, size_t firstItem);   // a nested container ended: advance its parent
        std::string m_HoverPanel;
        std::unordered_map<std::string, float> m_ScrollOffset, m_ScrollContent;

        // interaction
        std::string m_ActiveId;                  // pressed on this widget, button still down
        std::string m_RightActiveId;
        float m_PressX = 0, m_PressY = 0;
        bool m_Dragging = false, m_DragEnded = false, m_DragEndedLast = false;
        std::string m_DragId, m_DragIcon;
        std::string m_HotId, m_PrevHotId;        // hovered widget (tooltips)
        float m_HotTime = 0.0f;
        std::string m_TooltipText, m_PendingTooltip;
    };

} // namespace Engine
