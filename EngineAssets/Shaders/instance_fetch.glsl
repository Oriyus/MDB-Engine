// FetchInstance(k): the per-object data of entry k of this view's index list (DrawList), for lit.vert and
// shadow.vert (each declares struct Instance first). Desktop: two storage buffers; web: their textures.
#ifdef MDB_WEB
#include "web_storage.glsl"
uniform highp usampler2D u_SB0;      // the instances (256 bytes = 16 texels each)
uniform highp usampler2D u_SB1;      // the index lists
Instance FetchInstance(int k) {
    int t = int(SB_Word(u_SB1, k)) * 16;
    Instance i;
    i.Model = SB_Mat4(u_SB0, t);
    i.Normal = SB_Mat4(u_SB0, t + 4);
    i.Color = SB_Vec4(u_SB0, t + 8);
    i.Surface = SB_Vec4(u_SB0, t + 9);
    i.Emissive = SB_Vec4(u_SB0, t + 10);
    i.Info = SB_IVec4(u_SB0, t + 11);
    i.PrevModel = SB_Mat4(u_SB0, t + 12);
    return i;
}
#else
layout(std430, binding = 0) readonly buffer Instances { Instance u_Instances[]; };
layout(std430, binding = 1) readonly buffer InstanceIndices { uint u_Indices[]; };   // per view: which instances
Instance FetchInstance(int k) { return u_Instances[u_Indices[k]]; }
#endif
