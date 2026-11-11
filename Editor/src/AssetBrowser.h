#pragma once
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The Assets panel (like Unity's Project window or Godot's FileSystem dock): a folder tree, and the
// current folder's files as tiles or a list. It only browses and edits files; what opening or placing
// an asset means is up to the app, which gets Actions back from Draw.
//
// Paths are relative to the asset folder with forward slashes ("Models/Fox.glb"), the same strings
// scene files and scripts use. Dragging a file carries its path as a drag-and-drop payload of type
// AssetBrowser::Payload (see PayloadPath).
class AssetBrowser {
public:
    enum class Kind { Folder, Model, Texture, Prefab, Script, Sound, Scene, Environment, Shader, Dialogue, AnimGraph, UILayout, Other };

    struct Action {
        enum Type { OpenScene, Place, PlaySound, SetSky, OpenExternal, OpenDialogue, OpenAnimGraph, OpenUILayout } What;
        std::string Path;              // relative to the asset folder
    };

    static constexpr const char* Payload = "MDB_ASSET";

    explicit AssetBrowser(std::string assetDir);

    // Draws the panel. thumbnail(path) returns a GL texture for an image file (0 = none yet).
    // The returned actions are what the user asked for this frame (double-clicks, menu items).
    std::vector<Action> Draw(bool* open, const std::function<uint32_t(const std::string&)>& thumbnail);

    static Kind KindOf(const std::string& path);
    static const char* KindName(Kind k);
    // The asset path carried by a drag-and-drop payload of type Payload
    static std::string PayloadPath(const ImGuiPayload* payload);
    // Accept an asset dropped on the last item, if it is one of `kinds`. Empty if none.
    static std::string AcceptDrop(std::initializer_list<Kind> kinds);

    std::string Absolute(const std::string& rel) const { return m_AssetDir + rel; }
    // Show a folder ("Textures"; "" = the asset root)
    void SetFolder(const std::string& folder) { m_Folder = folder; m_Search[0] = '\0'; m_ForceScan = true; }

private:
    struct Entry {
        std::string Name;              // "Fox.glb"
        std::string Path;              // "Models/Fox.glb"
        Kind Type = Kind::Other;
        uintmax_t Size = 0;
    };

    void Rescan();
    void DrawFolderTree(const std::string& folder, int depth);
    void DrawTile(const Entry& e, float size, const std::function<uint32_t(const std::string&)>& thumbnail);
    void DrawListRow(const Entry& e);
    void HandleItem(const Entry& e);               // selection, double-click, drag source, context menu
    void DrawIcon(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, const Entry& e,
                  const std::function<uint32_t(const std::string&)>& thumbnail);
    void DrawDialogs();
    std::string UniqueName(const std::string& folder, const std::string& base, const std::string& ext) const;

    std::string m_AssetDir;            // absolute, ending in '/'
    std::string m_Folder;              // current folder, relative ("" = the asset root)
    std::string m_Selected;            // relative path of the selected entry
    std::vector<Entry> m_Entries;      // the current folder's contents (folders first)
    std::vector<std::string> m_Folders;// every folder, for the tree (relative)
    double m_LastScan = -1.0;
    bool m_ForceScan = true;
    float m_TileSize = 84.0f;
    bool m_ListView = false;
    char m_Search[64] = {};
    std::vector<Action> m_Actions;

    // Rename / delete dialogs
    std::string m_RenameTarget, m_DeleteTarget;
    char m_RenameBuf[128] = {};
    bool m_OpenRename = false, m_OpenDelete = false;
    std::string m_Status;              // the last file operation's result, shown in the footer
};
