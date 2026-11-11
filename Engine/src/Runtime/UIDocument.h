#pragma once
#include "Runtime/UILayout.h"

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    // A UI layout made in the editor's UI Editor and saved as Assets/UI/<name>.json: a tree of laid-out widgets
    // (the same ones scripts place with ui.begin_panel, ui.label ...), drawn every frame by ui.show(path).
    //
    //   { "version": 1, "preview": { sample values for the editor }, "widgets": [ panel nodes ] }
    //   node: { "type": "panel", "name": "hud", <properties>, "children": [ nodes ] }
    //
    // Types and their properties (all optional):
    //   panel   x y (0..1 of the view), anchor (center, top, topleft ...), w h (units; 0 = fit), title, close,
    //           padding, color, alpha, image, slice, on_close
    //   row     spacing          column  width          scroll  height
    //   label   text, size, color, align (left / center / right), bold, muted, wrap, shadow, width
    //   button  text, w, h, fill, size, color, disabled, selected, on_click, event
    //   image   path, w, h, color, alpha
    //   bar     value (0..1), w, h, color, back, text
    //   slot    icon, count, size, highlight, on_click, event
    //   space   h                separator
    // Every node: name, visible (a condition), repeat (a count or a list: the node is drawn once per entry, with
    // `index` and `item` set for its expressions), tooltip (shown on hover).
    //
    // Values can be Lua: a text takes markup and {=expr}; a number or true/false property may be a string,
    // which is an expression; visible / disabled / selected are conditions; on_click / on_close are statements.
    // They run in the calling script's environment (ui.show(path, { vars }) adds names on top).
    struct UINode {
        std::string Type = "label";
        std::string Name;
        nlohmann::json Props = nlohmann::json::object();   // everything else of the node
        std::vector<UINode> Children;
        uint32_t Id = 0;                    // runtime only: tells nodes apart in the editor (not saved)

        bool IsContainer() const { return Type == "panel" || Type == "row" || Type == "column" || Type == "scroll"; }
    };

    struct UIDocument {
        static constexpr int kVersion = 1;
        std::vector<UINode> Widgets;        // top level: panels
        nlohmann::json Preview = nlohmann::json::object();   // sample values for the editor's preview

        nlohmann::json ToJson() const;
        static bool FromJson(const nlohmann::json& j, UIDocument& out, std::string* error = nullptr);
        static std::shared_ptr<UIDocument> Load(const std::string& file, std::string* error = nullptr);
        bool Save(const std::string& file) const;        // through a temp file

        void AssignIds();                   // every node a fresh id (after loading)
        uint32_t NextId() const;            // one above the largest in use
        UINode* Find(uint32_t id);
        const UINode* Find(uint32_t id) const;
        // Things that look wrong: top-level widgets that aren't panels, containers with nothing in them, buttons
        // without an action or name, repeated names, images that aren't there
        std::vector<std::string> Issues(const std::string& assetDir) const;
    };

    // Where the expressions of a document get their values (the script engine; the editor's preview)
    class UIDocEvaluator {
    public:
        virtual ~UIDocEvaluator() = default;
        virtual std::string Text(const std::string& text) = 0;            // {=expr} replaced
        virtual bool Bool(const std::string& expr, bool fallback) = 0;
        virtual float Number(const std::string& expr, float fallback) = 0;
        virtual int BeginRepeat(const std::string& expr) = 0;             // how many times
        virtual void SetRepeat(int index) = 0;                            // 1..n: `index` and `item`
        virtual void EndRepeat() = 0;
        virtual void Run(const std::string& code) = 0;                    // on_click / on_close
        virtual void Event(const std::string& name, const UINode& node, int index) { (void)name; (void)node; (void)index; }
    };

    // A node's place on screen in the frame just drawn (0..1 of the view). Repeated nodes have one per copy.
    struct UINodeRect {
        uint32_t Id = 0;
        float X = 0, Y = 0, W = 0, H = 0;
        int Depth = 0;                      // 0 = a panel
        size_t First = SIZE_MAX;            // while drawing: its first item and where that item was (rows move
        float FirstY = 0.0f;                // their children down afterwards, FinishUIRects follows)
    };

    struct UIDocResult {
        std::string Clicked;                // the name of the button / slot clicked (or "#<id>"), the panel closed
        int Index = 0;                      // its repeat index (0 when not repeated)
    };

    // Draw the document into `ui` (between its BeginFrame and EndFrame). `idPrefix` keeps its panels apart from
    // other documents' and scripts' (the path). `rects`, if given, gets every node's rectangle; it is only right
    // after ui.EndFrame (rows centre their children at their end), so call FinishRects then.
    UIDocResult DrawUIDocument(UIContext& ui, const UIDocument& doc, UIDocEvaluator& eval, const std::string& idPrefix,
        const std::vector<ScriptUIItem>& items, std::vector<UINodeRect>* rects = nullptr);
    // Turn the item ranges DrawUIDocument noted into rectangles
    void FinishUIRects(const std::vector<ScriptUIItem>& items, std::vector<UINodeRect>& rects);

} // namespace Engine
