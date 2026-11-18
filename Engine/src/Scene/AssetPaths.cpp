#include "Scene/AssetPaths.h"
#include "Scene/Components.h"

namespace Engine {

    namespace {
        void Visit(MeshRendererComponent& c, const AssetPathVisitor& f) {
            for (MaterialSlot& s : c.Materials) if (!s.Path.empty()) f(s.Path, &s);
        }
        void Visit(TerrainComponent& c, const AssetPathVisitor& f) {
            if (!c.DataPath.empty()) f(c.DataPath, nullptr);
            for (TerrainLayer& l : c.Layers) {
                for (std::string* p : { &l.NormalMap, &l.RoughnessMap, &l.HeightMap }) if (!p->empty()) f(*p, nullptr);
                if (!l.LayerMaterial.Path.empty()) f(l.LayerMaterial.Path, &l.LayerMaterial);
            }
        }
        void Visit(AudioSourceComponent& c, const AssetPathVisitor& f) { if (!c.Clip.empty()) f(c.Clip, nullptr); }
        void Visit(ScriptComponent& c, const AssetPathVisitor& f) { if (!c.Path.empty()) f(c.Path, nullptr); }
        void Visit(PrefabComponent& c, const AssetPathVisitor& f) { if (!c.Path.empty()) f(c.Path, nullptr); }
        void Visit(AnimatorComponent& c, const AssetPathVisitor& f) {
            if (!c.Graph.empty()) f(c.Graph, nullptr);
            for (AnimEvent& e : c.Events) if (e.Name.rfind("sound:", 0) == 0) f(e.Name, nullptr);   // "sound:Sounds/x.wav"
        }

        template <class C>
        void VisitAll(Scene& scene, const AssetPathVisitor& f) {
            for (auto h : scene.Registry().view<C>()) Visit(scene.Registry().get<C>(h), f);
        }
    }

    void ForEachAssetPath(EntitySnapshot& e, const AssetPathVisitor& f) {
        if (e.MeshRenderer) Visit(*e.MeshRenderer, f);
        if (e.Terrain) Visit(*e.Terrain, f);
        if (e.AudioSource) Visit(*e.AudioSource, f);
        if (e.Script) Visit(*e.Script, f);
        if (e.Prefab) Visit(*e.Prefab, f);
        if (e.Animator) Visit(*e.Animator, f);
    }

    void ForEachAssetPath(Scene& scene, const AssetPathVisitor& f) {
        VisitAll<MeshRendererComponent>(scene, f);
        VisitAll<TerrainComponent>(scene, f);
        VisitAll<AudioSourceComponent>(scene, f);
        VisitAll<ScriptComponent>(scene, f);
        VisitAll<PrefabComponent>(scene, f);
        VisitAll<AnimatorComponent>(scene, f);
    }

} // namespace Engine
