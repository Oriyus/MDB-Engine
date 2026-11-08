#pragma once
#include <string>

// Small Windows shell helpers for the editor. Kept in their own file so <windows.h> and its macros
// (min, max, near, far...) never reach the rest of the editor. Paths are absolute, UTF-8.
namespace Platform {

    // Open a file with its default program (a .lua script in the code editor, a .png in the viewer)
    bool OpenWithSystem(const std::string& path);

    // Open Explorer at a folder, or at a file's folder with the file selected
    void ShowInExplorer(const std::string& path);

    // Delete a file or folder to the Recycle Bin (so it can be restored). False if it failed.
    bool MoveToRecycleBin(const std::string& path);

} // namespace Platform
