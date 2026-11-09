#pragma once
#include <functional>
#include <string>
#include <vector>

// Small Windows shell helpers for the editor. Kept in their own file so <windows.h> and its macros
// (min, max, near, far...) never reach the rest of the editor. Paths are absolute, UTF-8.
namespace Platform {

    // Open a file with its default program (a .lua script in the code editor, a .png in the viewer)
    bool OpenWithSystem(const std::string& path);

    // Open Explorer at a folder, or at a file's folder with the file selected
    void ShowInExplorer(const std::string& path);

    // Delete a file or folder to the Recycle Bin (so it can be restored). False if it failed.
    bool MoveToRecycleBin(const std::string& path);

    // The folder picker. Returns the chosen folder (absolute, UTF-8), or "" if cancelled.
    std::string PickFolder(const std::string& title, const std::string& startIn = "");

    // The folder this executable is in (absolute, UTF-8, '/'-separated, no trailing slash)
    std::string ExeDir();

    // Start this executable again with these arguments (each quoted as needed). False if it failed.
    bool LaunchSelf(const std::vector<std::string>& args);

    // Run a command line with no window, calling onLine for every line it prints (stdout and stderr,
    // on the calling thread). Blocks until it ends; returns its exit code, or -1 if it could not start.
    int RunProcess(const std::string& commandLine, const std::function<void(const std::string&)>& onLine);

} // namespace Platform
