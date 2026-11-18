#pragma once
#include "Scene/Scene.h"

#include <functional>
#include <string>

namespace Engine {

    // Every asset path that entity components keep as text, for the editor's rename / move of assets
    // (Editor/src/AssetReferences). Resources held by pointer (geometry, textures, cookies) are named through
    // ResourceLibrary, which re-keys its own caches (ResourceLibrary::RenameAssets), so they need nothing here.
    // `slot` is the MaterialSlot a path belongs to (its Asset may need loading again), else null.
    // Adding a component with a path field: list it in AssetPaths.cpp too.
    using AssetPathVisitor = std::function<void(std::string& path, MaterialSlot* slot)>;

    void ForEachAssetPath(EntitySnapshot& e, const AssetPathVisitor& f);
    void ForEachAssetPath(Scene& scene, const AssetPathVisitor& f);

} // namespace Engine
