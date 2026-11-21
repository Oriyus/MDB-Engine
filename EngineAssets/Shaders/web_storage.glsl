// Storage buffers in the web build (WebGL2 has none). Engine/src/Renderer/WebGL.cpp keeps each buffer as an RGBA32UI
// texture, 1024 texels (16 bytes each) a row, bound on the unit mdbgl_StorageUnit gives its binding point; a shader
// declares `uniform highp usampler2D u_SB<binding>;` (Shader.cpp points it at that unit) and reads the std430 data
// with these: texel t = bytes 16t..16t+15, word w = bytes 4w..4w+3.
#if defined(MDB_WEB) && !defined(MDB_WEB_STORAGE)
#define MDB_WEB_STORAGE
uvec4 SB_Texel(highp usampler2D s, int t) { return texelFetch(s, ivec2(t & 1023, t >> 10), 0); }
vec4  SB_Vec4(highp usampler2D s, int t)  { return uintBitsToFloat(SB_Texel(s, t)); }
ivec4 SB_IVec4(highp usampler2D s, int t) { return ivec4(SB_Texel(s, t)); }
mat4  SB_Mat4(highp usampler2D s, int t)  { return mat4(SB_Vec4(s, t), SB_Vec4(s, t + 1), SB_Vec4(s, t + 2), SB_Vec4(s, t + 3)); }
uint  SB_Word(highp usampler2D s, int w) {
    uvec4 v = SB_Texel(s, w >> 2);
    int c = w & 3;
    return c == 0 ? v.x : (c == 1 ? v.y : (c == 2 ? v.z : v.w));
}
uvec2 SB_UVec2(highp usampler2D s, int i) {   // an array of uvec2 (8 bytes each)
    uvec4 v = SB_Texel(s, i >> 1);
    return (i & 1) == 0 ? v.xy : v.zw;
}
#endif
