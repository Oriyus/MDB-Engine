#pragma once
#include "Core/Project.h"
#include <string>
#include <vector>

// Which project the editor works on, and the recent-projects list.
//
// The editor opens one project per run; opening another restarts the editor with --project <folder>
// (every panel, the resources and the play-mode systems are built for one Assets folder).
// The project is, in order:
//   --project <folder> on the command line
//   MDB_PROJECT=<folder> (test runs; a name like "Examples" means that folder of the source tree)
//   the most recently opened project that still exists (%APPDATA%/MDB Engine/projects.json)
//   the source tree's Examples project
namespace Projects {

    const Engine::Project& Current();          // resolved on first use
    Engine::Project& CurrentMutable();         // for Build Game's settings (saved with Project::Save)
    const std::string& EngineDir();            // the engine's files: shaders, Scripts/Lib (absolute, '/'-ended)
    bool IsExamples(const Engine::Project& project);

    std::vector<std::string> Recent();         // project folders, newest first (only ones that still exist)
    void AddRecent(const std::string& root);   // to the front (at most 10 are kept)
    void RemoveRecent(const std::string& root);

} // namespace Projects
