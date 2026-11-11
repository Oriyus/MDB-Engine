#pragma once
#include "Runtime/UIDocument.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    // The UI Editor's stand-in for a running script: a Lua state of its own whose globals are the document's
    // preview values (UIDocument::Preview, e.g. { "game": { "gold": 120 }, "hp": 7 }), so the layout's
    // expressions show real text, bars, counts and repeats while designing. In "try it" mode clicked buttons
    // run their on_click on those values too (they change until Reset or the values are edited).
    class UIPreview final : public UIDocEvaluator {
    public:
        UIPreview();
        ~UIPreview() override;

        void SetVars(const nlohmann::json& vars);   // rebuilds the globals if they differ from the last ones
        void Reset();                               // the values as given again
        void BeginFrame();                          // forget last frame's errors
        const std::vector<std::string>& Errors() const { return m_Errors; }   // this frame's, each once
        bool RunActions = false;                    // on_click / on_close change the values (else only noted)
        std::vector<std::string> Ran;               // what was clicked and run, newest last (the editor shows it)

        std::string Text(const std::string& text) override;
        bool Bool(const std::string& expr, bool fallback) override;
        float Number(const std::string& expr, float fallback) override;
        int BeginRepeat(const std::string& expr) override;
        void SetRepeat(int index) override;
        void EndRepeat() override;
        void Run(const std::string& code) override;
        void Event(const std::string& name, const UINode& node, int index) override;

        struct Impl;
    private:
        std::unique_ptr<Impl> m;
        std::vector<std::string> m_Errors;
        std::string m_VarsText;
        nlohmann::json m_Vars;
    };

} // namespace Engine
