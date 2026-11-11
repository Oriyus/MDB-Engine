#pragma once
#include "Runtime/GameDatabase.h"
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The Database window (Window > Database, or double-click Data/database.json in Assets): the game's design data
// (Engine::GameDatabase) in three tabs. Items: name, icon, model, kind, equipment slot, value, stacking, their
// own stats and their effects (on use / while equipped / on hit). NPCs: name, model and height, animation graph,
// script, dialogue, body, stats, and the starting inventory (with chances, what drops when it dies, what is
// equipped). Stats: the stats every NPC has (defaults, ranges, a ceiling stat) and which one kills at its minimum.
//
// Left: the list (search, + New, right-click: duplicate / delete; drag an NPC or item into the Viewport to place
// it). Right: the selected entry. Renaming an id updates the references inside the database (inventories,
// effects, stat values); scenes that use an NPC id keep the old one. Its own undo (Ctrl+Z / Ctrl+Y), Ctrl+S,
// Ctrl+D and Delete while the window has focus. Every change goes live at once (OnChanged), so a running game
// uses it; Save writes Assets/Data/database.json.
class DatabaseEditor {
public:
    static constexpr const char* NpcPayload = "MDB_DB_NPC";    // drag and drop: the id, as a string
    static constexpr const char* ItemPayload = "MDB_DB_ITEM";

    explicit DatabaseEditor(std::string assetDir);

    void Draw(bool* open);
    bool IsFocused() const { return m_Focused; }
    bool IsDirty() const;
    bool Save();
    void Reload();                             // the file again (unsaved changes are lost)
    const Engine::GameDatabase& Data() { EnsureLoaded(); return m_Db; }
    void SelectNpc(const std::string& id);     // show it (the window is the app's to open)
    void SelectItem(const std::string& id);

    std::function<void(std::shared_ptr<const Engine::GameDatabase>)> OnChanged;   // every edit, and when loaded
    std::function<void(const std::string& path)> OnOpenDialogue;    // "Dialogues/x.json"
    std::function<void(const std::string& path)> OnOpenAnimGraph;   // "AnimGraphs/x.json"
    std::function<void(const std::string& npcId)> OnPlaceNpc;       // "Place in scene"
    std::function<uint32_t(const std::string& path)> TextureId;     // icon previews (GL texture, 0 = none)

private:
    enum class Tab { Items, Npcs, Stats };

    void EnsureLoaded();
    void Commit();
    void Undo();
    void Redo();
    void Publish();

    void DrawMenuBar();
    void DrawList();
    void DrawItem(Engine::ItemDef& it);
    void DrawNpc(Engine::NpcDef& n);
    void DrawStat(Engine::StatDef& s);
    void DrawProblems();

    // widgets
    bool Text(const char* label, std::string& s, bool multiline = false);
    bool IdField(const char* what, std::string& id);       // rename on Enter / leaving the field
    bool AssetCombo(const char* label, std::string& path, const std::vector<const char*>& folders, int kinds);
    bool StatCombo(const char* label, std::string& stat, const char* none = nullptr);
    bool ItemCombo(const char* label, std::string& item);
    bool PresetText(const char* label, std::string& s, const std::vector<const char*>& presets);
    void Icon(const std::string& path, float size);

    void RenameItem(const std::string& from, const std::string& to);
    void RenameStat(const std::string& from, const std::string& to);
    template<typename T> std::string UniqueId(const std::vector<T>& list, const std::string& base) const;
    void NewEntry();
    void DuplicateEntry(int index);
    void DeleteEntry(int index);
    int& Selected();
    int Count() const;

    std::string m_AssetDir;
    Engine::GameDatabase m_Db;
    bool m_Loaded = false;
    std::string m_SavedText, m_PublishedText;
    std::vector<std::string> m_History;
    int m_HistoryPos = -1;

    Tab m_Tab = Tab::Items;
    Tab m_ForceTab = Tab::Items;
    bool m_ForceTabNext = false;
    int m_SelItem = -1, m_SelNpc = -1, m_SelStat = -1;
    char m_Search[64] = {};
    std::string m_IdEdit;                      // the id field being typed (applied when done)
    const void* m_IdEditOwner = nullptr;
    bool m_Focused = false;
    std::string m_Status;
    std::vector<std::string> m_Problems;
    int m_ProblemsFrame = 0;
};
