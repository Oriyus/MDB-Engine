// The desktop GL 4.5 calls the engine uses, on WebGL2 (Emscripten builds only). See Engine/web/glad/glad.h.
#ifdef __EMSCRIPTEN__

#include <glad/glad.h>
#include <emscripten.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// The real ES entry points (the header redirects these names to mdbgl_*)
#undef glBindFramebuffer
#undef glBindVertexArray
#undef glUseProgram
#undef glDrawArrays
#undef glDrawArraysInstanced
#undef glDrawElements
#undef glDrawElementsInstanced
#undef glEnable
#undef glDisable
#undef glIsEnabled
#undef glGetIntegerv
#undef glBindBufferBase
#undef glDeleteTextures
#undef glDeleteBuffers
#undef glDeleteVertexArrays
#undef glDeleteFramebuffers

namespace {

    constexpr GLuint kScratchUnit = 31;           // DSA texture edits happen here (the binding is put back)
    constexpr int kUnits = 32;
    constexpr size_t kRowTexels = 1024;           // storage textures: texels per row
    constexpr size_t kRowBytes = kRowTexels * 16; // RGBA32UI

    void WarnOnce(const char* what) {
        static std::unordered_set<std::string> seen;
        if (seen.insert(what).second) std::printf("[web] %s is not available in WebGL2 (ignored)\n", what);
    }

    struct TexInfo {
        GLenum Target = GL_TEXTURE_2D;
        GLenum Format = 0;                    // internal format (TextureStorage)
        GLsizei W = 0, H = 0, D = 0;
        GLint MinFilter = GL_NEAREST_MIPMAP_LINEAR, MagFilter = GL_LINEAR;
        GLint Compare = GL_NONE;
    };
    std::unordered_map<GLuint, TexInfo> g_Tex;

    // Texture bindings per unit and target, as this file set them
    int TargetSlot(GLenum t) {
        switch (t) { case GL_TEXTURE_2D: return 0; case GL_TEXTURE_CUBE_MAP: return 1; case GL_TEXTURE_2D_ARRAY: return 2; case GL_TEXTURE_3D: return 3; default: return 0; }
    }
    const GLenum kTargets[4] = { GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D };
    GLuint g_Bound[kUnits][4] = {};
    GLuint g_ActiveUnit = 0;

    void ActiveUnit(GLuint unit) {
        if (unit != g_ActiveUnit) { glActiveTexture(GL_TEXTURE0 + unit); g_ActiveUnit = unit; }
    }
    void BindOn(GLuint unit, GLenum target, GLuint tex) {
        ActiveUnit(unit);
        glBindTexture(target, tex);
        g_Bound[unit][TargetSlot(target)] = tex;
    }
    TexInfo& Info(GLuint tex) { return g_Tex[tex]; }
    // Bind on the scratch unit for an edit; Restore() puts back what was there
    struct ScratchBind {
        GLenum Target;
        GLuint Prev, PrevUnit;
        explicit ScratchBind(GLuint tex) {
            Target = Info(tex).Target;
            PrevUnit = g_ActiveUnit;
            Prev = g_Bound[kScratchUnit][TargetSlot(Target)];
            ActiveUnit(kScratchUnit);
            glBindTexture(Target, tex);
        }
        ~ScratchBind() {
            glBindTexture(Target, Prev);
            ActiveUnit(PrevUnit);
        }
    };

    bool IsDepthFormat(GLenum f) {
        return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH_COMPONENT32F ||
               f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8;
    }
    bool IsDepthStencil(GLenum f) { return f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8; }

    // ES 3.0: a depth texture sampled without comparison is incomplete unless it filters NEAREST
    void ApplyFilters(GLuint tex) {
        TexInfo& t = Info(tex);
        GLint mn = t.MinFilter, mg = t.MagFilter;
        if (IsDepthFormat(t.Format) && t.Compare == GL_NONE) {
            mg = GL_NEAREST;
            mn = (mn == GL_NEAREST_MIPMAP_NEAREST || mn == GL_NEAREST) ? mn : GL_NEAREST;
        }
        glTexParameteri(t.Target, GL_TEXTURE_MIN_FILTER, mn);
        glTexParameteri(t.Target, GL_TEXTURE_MAG_FILTER, mg);
    }

    // ---- framebuffers
    GLuint g_DrawFB = 0, g_ReadFB = 0;
    // What each framebuffer has attached (attachment -> texture): WebGL refuses a draw while a texture attached to the
    // framebuffer drawn into is bound on a unit an active sampler uses, even if that path never reads it
    std::unordered_map<GLuint, std::unordered_map<GLenum, GLuint>> g_Attached;
    void NoteAttached(GLuint fb, GLenum attachment, GLuint tex) {
        if (tex) g_Attached[fb][attachment] = tex; else g_Attached[fb].erase(attachment);
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) { g_Attached[fb].erase(GL_DEPTH_ATTACHMENT); g_Attached[fb].erase(GL_STENCIL_ATTACHMENT); }
    }
    struct DrawFBScope {
        GLuint Prev;
        explicit DrawFBScope(GLuint fb) : Prev(g_DrawFB) { if (fb != g_DrawFB) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb); }
        ~DrawFBScope() { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Prev); }
    };
    struct ReadFBScope {
        GLuint Prev;
        explicit ReadFBScope(GLuint fb) : Prev(g_ReadFB) { glBindFramebuffer(GL_READ_FRAMEBUFFER, fb); }
        ~ReadFBScope() { glBindFramebuffer(GL_READ_FRAMEBUFFER, Prev); }
    };

    // ---- buffers
    std::unordered_map<GLuint, GLenum> g_RealBuffers;   // vertex / index buffers (marked by Buffer.cpp)
    struct Storage {
        std::vector<uint8_t> Data;            // padded to whole rows
        size_t Size = 0;                      // bytes the engine asked for
        GLuint Tex = 0;
        size_t Rows = 0;                      // texture height
        size_t DirtyLo = SIZE_MAX, DirtyHi = 0;
    };
    std::unordered_map<GLuint, Storage> g_Storage;

    void Dirty(Storage& s, size_t lo, size_t hi) { s.DirtyLo = std::min(s.DirtyLo, lo); s.DirtyHi = std::max(s.DirtyHi, hi); }
    void Resize(Storage& s, size_t size) {
        s.Size = size;
        const size_t rows = std::max<size_t>(1, (size + kRowBytes - 1) / kRowBytes);
        s.Data.resize(rows * kRowBytes, 0);
    }

    // ---- vertex arrays
    struct AttribState {
        bool Enabled = false, HasFormat = false, Integer = false;
        GLint Size = 4;
        GLenum Type = GL_FLOAT;
        GLboolean Norm = GL_FALSE;
        GLuint Offset = 0;
        GLuint Binding = 0;
    };
    struct BindingState { GLuint Buffer = 0; GLintptr Offset = 0; GLsizei Stride = 0; };
    struct VaoState { AttribState Attribs[16]; BindingState Bindings[16]; };
    std::unordered_map<GLuint, VaoState> g_Vao;
    GLuint g_BoundVao = 0;

    void ApplyAttrib(GLuint vao, GLuint a) {
        VaoState& v = g_Vao[vao];
        const AttribState& s = v.Attribs[a];
        if (!s.HasFormat || s.Binding >= 16) return;
        const BindingState& b = v.Bindings[s.Binding];
        if (!b.Buffer) return;
        if (g_BoundVao != vao) glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, b.Buffer);
        const void* ptr = (const void*)(intptr_t)(b.Offset + (GLintptr)s.Offset);
        if (s.Integer) glVertexAttribIPointer(a, s.Size, s.Type, b.Stride, ptr);
        else glVertexAttribPointer(a, s.Size, s.Type, s.Norm, b.Stride, ptr);
        if (s.Enabled) glEnableVertexAttribArray(a);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (g_BoundVao != vao) glBindVertexArray(g_BoundVao);
    }

    // ---- programs: the one the engine asked for (logical) and the one WebGL has (actual)
    GLuint g_Program = 0, g_ActualProgram = 0;
    void ActualProgram(GLuint p) { if (p != g_ActualProgram) { glUseProgram(p); g_ActualProgram = p; } }
    void SyncProgram() { ActualProgram(g_Program); }

    bool g_Scissor = false;
} // namespace

// ---- OES_draw_buffers_indexed (per-attachment colour mask and blend function)
EM_JS(int, mdbgl_js_HasIndexed, (), {
    if (GLctx.mdbIndexed === undefined) GLctx.mdbIndexed = GLctx.getExtension('OES_draw_buffers_indexed') || null;
    return GLctx.mdbIndexed ? 1 : 0;
});
EM_JS(void, mdbgl_js_ColorMaski, (int i, int r, int g, int b, int a), {
    GLctx.mdbIndexed.colorMaskiOES(i, !!r, !!g, !!b, !!a);
});
EM_JS(void, mdbgl_js_BlendFunci, (int i, int s, int d), {
    GLctx.mdbIndexed.blendFunciOES(i, s, d);
});

int mdbgl_StorageUnit(GLuint binding) {
    // Units the web renderer leaves free (contact depth 1, terrain surface maps 14..17, the shadow atlas 18 / 19,
    // cookies 20, ambient occlusion in lighting 21 are desktop-only)
    switch (binding) {
    case 0: return 14;     // instances
    case 1: return 15;     // instance indices
    case 2: return 21;     // particles
    case 3: return 22;     // decals (their textures use unit 1; graph textures start here but decals draw alone)
    case 4: return 16;     // grass clumps
    case 5: return 17;     // lights
    case 6: return 18;     // light cluster cells
    case 7: return 19;     // light cluster indices
    case 10: return 20;    // grass ground shade
    default: return -1;    // 8 (GPU binning stats), 9 (shadow atlas squares): not on the web
    }
}

void mdbgl_MarkBuffer(GLuint buffer, GLenum target) {
    g_RealBuffers[buffer] = target;
    g_Storage.erase(buffer);
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        // WebGL fixes a buffer's kind at its first bind: bind it as indices once (on no vertex array's behalf)
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        glBindVertexArray(g_BoundVao);
    }
    else {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

// ---- creation
void mdbgl_CreateTextures(GLenum target, GLsizei n, GLuint* ids) {
    glGenTextures(n, ids);
    for (GLsizei i = 0; i < n; i++) {
        TexInfo& t = Info(ids[i]);
        t = TexInfo{};
        t.Target = target;
        ScratchBind b(ids[i]);                // a first bind gives the texture its target
    }
}
void mdbgl_CreateBuffers(GLsizei n, GLuint* ids) {
    glGenBuffers(n, ids);                     // the name only: storage buffers never get WebGL data
    for (GLsizei i = 0; i < n; i++) g_Storage[ids[i]] = Storage{};
}
void mdbgl_CreateFramebuffers(GLsizei n, GLuint* ids) { glGenFramebuffers(n, ids); }
void mdbgl_CreateVertexArrays(GLsizei n, GLuint* ids) {
    glGenVertexArrays(n, ids);
    for (GLsizei i = 0; i < n; i++) g_Vao[ids[i]] = VaoState{};
}
void mdbgl_CreateSamplers(GLsizei n, GLuint* ids) { glGenSamplers(n, ids); }

void mdbgl_DeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) {
        for (int u = 0; u < kUnits; u++) for (int t = 0; t < 4; t++) if (g_Bound[u][t] == ids[i]) g_Bound[u][t] = 0;
        g_Tex.erase(ids[i]);
    }
    glDeleteTextures(n, ids);
}
void mdbgl_DeleteBuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_Storage.find(ids[i]);
        if (it != g_Storage.end()) {
            if (it->second.Tex) mdbgl_DeleteTextures(1, &it->second.Tex);
            g_Storage.erase(it);
        }
        g_RealBuffers.erase(ids[i]);
    }
    glDeleteBuffers(n, ids);
}
void mdbgl_DeleteVertexArrays(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) { g_Vao.erase(ids[i]); if (g_BoundVao == ids[i]) g_BoundVao = 0; }
    glDeleteVertexArrays(n, ids);
}
void mdbgl_DeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) {
        if (g_DrawFB == ids[i]) g_DrawFB = 0;
        if (g_ReadFB == ids[i]) g_ReadFB = 0;
    }
    glDeleteFramebuffers(n, ids);
}

// ---- textures
void mdbgl_TextureStorage2D(GLuint tex, GLsizei levels, GLenum format, GLsizei w, GLsizei h) {
    TexInfo& t = Info(tex);
    t.Format = format; t.W = w; t.H = h; t.D = 1;
    ScratchBind b(tex);
    glTexStorage2D(t.Target, levels, format, w, h);
    ApplyFilters(tex);
}
void mdbgl_TextureStorage3D(GLuint tex, GLsizei levels, GLenum format, GLsizei w, GLsizei h, GLsizei d) {
    TexInfo& t = Info(tex);
    t.Format = format; t.W = w; t.H = h; t.D = d;
    ScratchBind b(tex);
    glTexStorage3D(t.Target, levels, format, w, h, d);
    ApplyFilters(tex);
}
void mdbgl_TextureSubImage2D(GLuint tex, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels) {
    ScratchBind b(tex);
    glTexSubImage2D(b.Target, level, x, y, w, h, format, type, pixels);
}
void mdbgl_TextureSubImage3D(GLuint tex, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void* pixels) {
    ScratchBind b(tex);
    if (b.Target == GL_TEXTURE_CUBE_MAP) {        // DSA addresses cube faces as layers
        for (GLsizei f = 0; f < d; f++)
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + z + f, level, x, y, w, h, format, type, pixels);
        return;
    }
    glTexSubImage3D(b.Target, level, x, y, z, w, h, d, format, type, pixels);
}
void mdbgl_TextureParameteri(GLuint tex, GLenum pname, GLint v) {
    TexInfo& t = Info(tex);
    ScratchBind b(tex);
    if (pname == GL_TEXTURE_MIN_FILTER) { t.MinFilter = v; ApplyFilters(tex); return; }
    if (pname == GL_TEXTURE_MAG_FILTER) { t.MagFilter = v; ApplyFilters(tex); return; }
    if (pname == GL_TEXTURE_COMPARE_MODE) { t.Compare = v; glTexParameteri(t.Target, pname, v); ApplyFilters(tex); return; }
    if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R) && v == GL_CLAMP_TO_BORDER) v = GL_CLAMP_TO_EDGE;
    glTexParameteri(t.Target, pname, v);
}
void mdbgl_TextureParameterf(GLuint tex, GLenum pname, GLfloat v) {
    ScratchBind b(tex);
    glTexParameterf(b.Target, pname, v);
}
void mdbgl_TextureParameterfv(GLuint, GLenum pname, const GLfloat*) {
    if (pname == GL_TEXTURE_BORDER_COLOR) return;   // no border colours: CLAMP_TO_BORDER became CLAMP_TO_EDGE
    WarnOnce("glTextureParameterfv");
}
void mdbgl_TextureParameteriv(GLuint, GLenum pname, const GLint*) {
    if (pname == GL_TEXTURE_SWIZZLE_RGBA) { WarnOnce("Texture swizzle"); return; }
    WarnOnce("glTextureParameteriv");
}
void mdbgl_GenerateTextureMipmap(GLuint tex) {
    ScratchBind b(tex);
    glGenerateMipmap(b.Target);
}
void mdbgl_BindTextureUnit(GLuint unit, GLuint tex) {
    if (unit >= (GLuint)kUnits) return;
    if (tex == 0) {
        for (int t = 0; t < 4; t++) if (g_Bound[unit][t]) BindOn(unit, kTargets[t], 0);
        return;
    }
    const GLenum target = Info(tex).Target;
    if (g_Bound[unit][TargetSlot(target)] == tex) return;
    BindOn(unit, target, tex);
}
void mdbgl_TextureView(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint) { WarnOnce("glTextureView"); }
void mdbgl_GetTextureImage(GLuint, GLint, GLenum, GLenum, GLsizei size, void* pixels) {
    WarnOnce("glGetTextureImage");
    if (pixels && size > 0) std::memset(pixels, 0, (size_t)size);
}

// A copy between two textures of the same format, through two scratch framebuffers and a blit
void mdbgl_CopyImageSubData(GLuint src, GLenum, GLint srcLevel, GLint sx, GLint sy, GLint sz,
                            GLuint dst, GLenum, GLint dstLevel, GLint dx, GLint dy, GLint dz,
                            GLsizei w, GLsizei h, GLsizei d) {
    static GLuint fbs[2] = { 0, 0 };
    if (!fbs[0]) glGenFramebuffers(2, fbs);
    const TexInfo& si = Info(src);
    const TexInfo& di = Info(dst);
    const bool depth = IsDepthFormat(si.Format);
    const GLenum att = !depth ? GL_COLOR_ATTACHMENT0 : IsDepthStencil(si.Format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    const GLbitfield mask = !depth ? GL_COLOR_BUFFER_BIT : IsDepthStencil(si.Format) ? (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : GL_DEPTH_BUFFER_BIT;
    const GLuint prevRead = g_ReadFB, prevDraw = g_DrawFB;
    const bool scissor = g_Scissor;
    if (scissor) glDisable(GL_SCISSOR_TEST);
    auto attach = [&](GLenum target, GLuint tex, const TexInfo& ti, GLint level, GLint layer) {
        if (ti.Target == GL_TEXTURE_2D_ARRAY || ti.Target == GL_TEXTURE_3D) glFramebufferTextureLayer(target, att, tex, level, layer);
        else if (ti.Target == GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(target, att, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, tex, level);
        else glFramebufferTexture2D(target, att, GL_TEXTURE_2D, tex, level);
    };
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbs[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbs[1]);
    for (GLsizei l = 0; l < std::max(d, 1); l++) {
        attach(GL_READ_FRAMEBUFFER, src, si, srcLevel, sz + l);
        attach(GL_DRAW_FRAMEBUFFER, dst, di, dstLevel, dz + l);
        glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, mask, GL_NEAREST);
    }
    // Detach, so the textures aren't attached anywhere they could be sampled from
    attach(GL_READ_FRAMEBUFFER, 0, si, 0, 0);
    attach(GL_DRAW_FRAMEBUFFER, 0, di, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void mdbgl_ClearTexImage(GLuint tex, GLint level, GLenum, GLenum, const void* data) {
    // Through a scratch framebuffer (colour textures; zero unless data says otherwise)
    static GLuint fb = 0;
    if (!fb) glGenFramebuffers(1, &fb);
    const TexInfo& t = Info(tex);
    DrawFBScope s(fb);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    float c[4] = { 0, 0, 0, 0 };
    if (data && (t.Format == GL_RGBA16F || t.Format == GL_RG16F || t.Format == GL_RGBA32F)) std::memcpy(c, data, 8);
    glClearBufferfv(GL_COLOR, 0, c);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

// ---- buffers
static GLenum RealTarget(GLuint buf) { auto it = g_RealBuffers.find(buf); return it == g_RealBuffers.end() ? 0 : it->second; }

void mdbgl_NamedBufferData(GLuint buf, GLsizeiptr size, const void* data, GLenum usage) {
    auto it = g_Storage.find(buf);
    if (it != g_Storage.end()) {
        Storage& s = it->second;
        Resize(s, (size_t)size);
        if (data) std::memcpy(s.Data.data(), data, (size_t)size);
        Dirty(s, 0, (size_t)size);
        return;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
    glBufferData(GL_COPY_WRITE_BUFFER, size, data, usage);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}
void mdbgl_NamedBufferStorage(GLuint buf, GLsizeiptr size, const void* data, GLbitfield) {
    mdbgl_NamedBufferData(buf, size, data, GL_DYNAMIC_DRAW);
}
void mdbgl_NamedBufferSubData(GLuint buf, GLintptr offset, GLsizeiptr size, const void* data) {
    auto it = g_Storage.find(buf);
    if (it != g_Storage.end()) {
        Storage& s = it->second;
        if ((size_t)(offset + size) > s.Size) Resize(s, (size_t)(offset + size));
        std::memcpy(s.Data.data() + offset, data, (size_t)size);
        Dirty(s, (size_t)offset, (size_t)(offset + size));
        return;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
    glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}
void mdbgl_ClearNamedBufferData(GLuint buf, GLenum, GLenum, GLenum, const void* data) {
    auto it = g_Storage.find(buf);
    if (it == g_Storage.end()) { WarnOnce("glClearNamedBufferData on a vertex buffer"); return; }
    Storage& s = it->second;
    uint32_t v = 0;
    if (data) std::memcpy(&v, data, 4);
    for (size_t i = 0; i + 4 <= s.Size; i += 4) std::memcpy(s.Data.data() + i, &v, 4);
    Dirty(s, 0, s.Size);
}
void mdbgl_CopyNamedBufferSubData(GLuint src, GLuint dst, GLintptr so, GLintptr dO, GLsizeiptr size) {
    auto a = g_Storage.find(src), b = g_Storage.find(dst);
    if (a != g_Storage.end() && b != g_Storage.end()) {
        if ((size_t)(dO + size) > b->second.Size) Resize(b->second, (size_t)(dO + size));
        std::memmove(b->second.Data.data() + dO, a->second.Data.data() + so, (size_t)size);
        Dirty(b->second, (size_t)dO, (size_t)(dO + size));
        return;
    }
    if (a == g_Storage.end() && b == g_Storage.end()) {
        glBindBuffer(GL_COPY_READ_BUFFER, src);
        glBindBuffer(GL_COPY_WRITE_BUFFER, dst);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, so, dO, size);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        return;
    }
    WarnOnce("Copies between storage and vertex buffers");
}
void* mdbgl_MapNamedBufferRange(GLuint, GLintptr, GLsizeiptr, GLbitfield) { WarnOnce("glMapNamedBufferRange"); return nullptr; }
GLboolean mdbgl_UnmapNamedBuffer(GLuint) { return GL_TRUE; }

void mdbgl_BindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    if (target != GL_SHADER_STORAGE_BUFFER) { glBindBufferBase(target, index, buffer); return; }
    const int unit = mdbgl_StorageUnit(index);
    if (unit < 0) return;
    auto it = g_Storage.find(buffer);
    if (it == g_Storage.end()) { BindOn((GLuint)unit, GL_TEXTURE_2D, 0); return; }
    Storage& s = it->second;
    if (s.Data.empty()) Resize(s, 16);
    const size_t rows = s.Data.size() / kRowBytes;
    if (!s.Tex || s.Rows < rows) {
        // (Re)make the texture to fit, then upload everything
        if (!s.Tex) {
            glGenTextures(1, &s.Tex);
            TexInfo& t = Info(s.Tex);
            t.Target = GL_TEXTURE_2D; t.Format = GL_RGBA32UI; t.MinFilter = t.MagFilter = GL_NEAREST;
        }
        s.Rows = std::max(rows, s.Rows * 2);     // grow by doubling (fewer re-creations)
        if (s.Data.size() < s.Rows * kRowBytes) s.Data.resize(s.Rows * kRowBytes, 0);
        TexInfo& t = Info(s.Tex);
        t.W = (GLsizei)kRowTexels; t.H = (GLsizei)s.Rows;
        BindOn((GLuint)unit, GL_TEXTURE_2D, s.Tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, (GLsizei)kRowTexels, (GLsizei)s.Rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, s.Data.data());
        s.DirtyLo = SIZE_MAX; s.DirtyHi = 0;
        return;
    }
    BindOn((GLuint)unit, GL_TEXTURE_2D, s.Tex);
    if (s.DirtyLo < s.DirtyHi) {
        const size_t r0 = s.DirtyLo / kRowBytes, r1 = std::min((s.DirtyHi + kRowBytes - 1) / kRowBytes, s.Rows);
        if (r1 > r0)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)r0, (GLsizei)kRowTexels, (GLsizei)(r1 - r0), GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                            s.Data.data() + r0 * kRowBytes);
        s.DirtyLo = SIZE_MAX; s.DirtyHi = 0;
    }
}

// ---- framebuffers
void mdbgl_BindFramebuffer(GLenum target, GLuint fb) {
    glBindFramebuffer(target, fb);
    if (target == GL_FRAMEBUFFER) g_DrawFB = g_ReadFB = fb;
    else if (target == GL_DRAW_FRAMEBUFFER) g_DrawFB = fb;
    else if (target == GL_READ_FRAMEBUFFER) g_ReadFB = fb;
}
void mdbgl_NamedFramebufferTexture(GLuint fb, GLenum attachment, GLuint tex, GLint level) {
    NoteAttached(fb, attachment, tex);
    DrawFBScope s(fb);
    if (tex && Info(tex).Target != GL_TEXTURE_2D) {
        // Layered attachments don't exist in ES: attach the first layer / face (the engine re-attaches per layer)
        const GLenum t = Info(tex).Target;
        if (t == GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X, tex, level);
        else glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, tex, level, 0);
        return;
    }
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, tex, level);
}
void mdbgl_NamedFramebufferTextureLayer(GLuint fb, GLenum attachment, GLuint tex, GLint level, GLint layer) {
    NoteAttached(fb, attachment, tex);
    DrawFBScope s(fb);
    if (tex && Info(tex).Target == GL_TEXTURE_CUBE_MAP)
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, tex, level);
    else
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, tex, level, layer);
}
void mdbgl_NamedFramebufferDrawBuffers(GLuint fb, GLsizei n, const GLenum* bufs) {
    DrawFBScope s(fb);
    if (fb == 0) { const GLenum back = GL_BACK; glDrawBuffers(1, &back); return; }
    glDrawBuffers(n, bufs);
}
void mdbgl_NamedFramebufferDrawBuffer(GLuint fb, GLenum buf) {
    DrawFBScope s(fb);
    if (fb == 0) buf = buf == GL_NONE ? GL_NONE : GL_BACK;
    glDrawBuffers(1, &buf);
}
void mdbgl_NamedFramebufferReadBuffer(GLuint fb, GLenum buf) {
    ReadFBScope s(fb);
    if (fb == 0 && buf != GL_NONE) buf = GL_BACK;
    glReadBuffer(buf);
}
GLenum mdbgl_CheckNamedFramebufferStatus(GLuint fb, GLenum) {
    DrawFBScope s(fb);
    return glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
}
void mdbgl_BlitNamedFramebuffer(GLuint read, GLuint draw, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    ReadFBScope r(read);
    DrawFBScope d(draw);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
void mdbgl_ClearNamedFramebufferfv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLfloat* v) { DrawFBScope s(fb); glClearBufferfv(buffer, drawbuffer, v); }
void mdbgl_ClearNamedFramebufferiv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLint* v) { DrawFBScope s(fb); glClearBufferiv(buffer, drawbuffer, v); }
void mdbgl_ClearNamedFramebufferuiv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLuint* v) { DrawFBScope s(fb); glClearBufferuiv(buffer, drawbuffer, v); }
void mdbgl_ClearNamedFramebufferfi(GLuint fb, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) { DrawFBScope s(fb); glClearBufferfi(buffer, drawbuffer, depth, stencil); }

// ---- vertex arrays
void mdbgl_BindVertexArray(GLuint vao) { glBindVertexArray(vao); g_BoundVao = vao; }
void mdbgl_VertexArrayVertexBuffer(GLuint vao, GLuint binding, GLuint buffer, GLintptr offset, GLsizei stride) {
    if (binding >= 16) return;
    VaoState& v = g_Vao[vao];
    v.Bindings[binding] = BindingState{ buffer, offset, stride };
    for (GLuint a = 0; a < 16; a++) if (v.Attribs[a].HasFormat && v.Attribs[a].Binding == binding) ApplyAttrib(vao, a);
}
void mdbgl_EnableVertexArrayAttrib(GLuint vao, GLuint attrib) {
    if (attrib >= 16) return;
    g_Vao[vao].Attribs[attrib].Enabled = true;
    if (g_BoundVao != vao) glBindVertexArray(vao);
    glEnableVertexAttribArray(attrib);
    if (g_BoundVao != vao) glBindVertexArray(g_BoundVao);
}
void mdbgl_VertexArrayAttribFormat(GLuint vao, GLuint attrib, GLint size, GLenum type, GLboolean norm, GLuint offset) {
    if (attrib >= 16) return;
    AttribState& a = g_Vao[vao].Attribs[attrib];
    a.HasFormat = true; a.Integer = false; a.Size = size; a.Type = type; a.Norm = norm; a.Offset = offset;
    ApplyAttrib(vao, attrib);
}
void mdbgl_VertexArrayAttribIFormat(GLuint vao, GLuint attrib, GLint size, GLenum type, GLuint offset) {
    if (attrib >= 16) return;
    AttribState& a = g_Vao[vao].Attribs[attrib];
    a.HasFormat = true; a.Integer = true; a.Size = size; a.Type = type; a.Offset = offset;
    ApplyAttrib(vao, attrib);
}
void mdbgl_VertexArrayAttribBinding(GLuint vao, GLuint attrib, GLuint binding) {
    if (attrib >= 16) return;
    g_Vao[vao].Attribs[attrib].Binding = binding;
    ApplyAttrib(vao, attrib);
}
void mdbgl_VertexArrayElementBuffer(GLuint vao, GLuint buffer) {
    if (g_BoundVao != vao) glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    if (g_BoundVao != vao) glBindVertexArray(g_BoundVao);
}

// ---- programs and uniforms
void mdbgl_UseProgram(GLuint program) { g_Program = program; ActualProgram(program); }
void mdbgl_ProgramUniform1i(GLuint p, GLint loc, GLint v) { if (loc < 0) return; ActualProgram(p); glUniform1i(loc, v); }
void mdbgl_ProgramUniform1f(GLuint p, GLint loc, GLfloat v) { if (loc < 0) return; ActualProgram(p); glUniform1f(loc, v); }
void mdbgl_ProgramUniform2fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v) { if (loc < 0) return; ActualProgram(p); glUniform2fv(loc, n, v); }
void mdbgl_ProgramUniform3fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v) { if (loc < 0) return; ActualProgram(p); glUniform3fv(loc, n, v); }
void mdbgl_ProgramUniform4fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v) { if (loc < 0) return; ActualProgram(p); glUniform4fv(loc, n, v); }
void mdbgl_ProgramUniform1fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v) { if (loc < 0) return; ActualProgram(p); glUniform1fv(loc, n, v); }
void mdbgl_ProgramUniform1iv(GLuint p, GLint loc, GLsizei n, const GLint* v) { if (loc < 0) return; ActualProgram(p); glUniform1iv(loc, n, v); }
void mdbgl_ProgramUniform4iv(GLuint p, GLint loc, GLsizei n, const GLint* v) { if (loc < 0) return; ActualProgram(p); glUniform4iv(loc, n, v); }
void mdbgl_ProgramUniformMatrix4fv(GLuint p, GLint loc, GLsizei n, GLboolean t, const GLfloat* v) { if (loc < 0) return; ActualProgram(p); glUniformMatrix4fv(loc, n, t, v); }

// ---- draws and state
// Unbind what the framebuffer being drawn into has attached (see g_Attached)
static void BreakFeedback() {
    if (g_DrawFB == 0) return;
    auto it = g_Attached.find(g_DrawFB);
    if (it == g_Attached.end()) return;
    for (const auto& [att, tex] : it->second) {
        const int slot = TargetSlot(Info(tex).Target);
        for (int u = 0; u < kUnits; u++) if (g_Bound[u][slot] == tex) BindOn((GLuint)u, kTargets[slot], 0);
    }
}
static void BeforeDraw() { SyncProgram(); BreakFeedback(); }

void mdbgl_DrawArrays(GLenum mode, GLint first, GLsizei count) { BeforeDraw(); glDrawArrays(mode, first, count); }
void mdbgl_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n) { BeforeDraw(); glDrawArraysInstanced(mode, first, count, n); }
void mdbgl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* idx) { BeforeDraw(); glDrawElements(mode, count, type, idx); }
void mdbgl_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* idx, GLsizei n) { BeforeDraw(); glDrawElementsInstanced(mode, count, type, idx, n); }

static bool Unsupported(GLenum cap) {
    return cap == GL_TEXTURE_CUBE_MAP_SEAMLESS || cap == GL_FRAMEBUFFER_SRGB || cap == GL_POLYGON_OFFSET_LINE;
}
void mdbgl_Enable(GLenum cap) { if (Unsupported(cap)) return; if (cap == GL_SCISSOR_TEST) g_Scissor = true; glEnable(cap); }
void mdbgl_Disable(GLenum cap) { if (Unsupported(cap)) return; if (cap == GL_SCISSOR_TEST) g_Scissor = false; glDisable(cap); }
GLboolean mdbgl_IsEnabled(GLenum cap) { return Unsupported(cap) ? GL_FALSE : glIsEnabled(cap); }
void mdbgl_GetIntegerv(GLenum pname, GLint* v) {
    if (pname == GL_CURRENT_PROGRAM) { *v = (GLint)g_Program; return; }
    if (pname == GL_DRAW_FRAMEBUFFER_BINDING) { *v = (GLint)g_DrawFB; return; }
    glGetIntegerv(pname, v);
}
void mdbgl_ColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (mdbgl_js_HasIndexed()) { mdbgl_js_ColorMaski((int)buf, r, g, b, a); return; }
    if (buf == 0) glColorMask(r, g, b, a);   // without the extension only the colour target's mask is kept
}
void mdbgl_BlendFunci(GLuint buf, GLenum src, GLenum dst) {
    if (mdbgl_js_HasIndexed()) { mdbgl_js_BlendFunci((int)buf, (int)src, (int)dst); return; }
    if (buf == 0) glBlendFunc(src, dst);     // the other targets are integer or not drawn: never blended
}
void mdbgl_PolygonMode(GLenum, GLenum) { WarnOnce("glPolygonMode (wireframe)"); }

void mdbgl_DispatchCompute(GLuint, GLuint, GLuint) { WarnOnce("Compute shaders"); }
void mdbgl_MemoryBarrier(GLbitfield) {}
void mdbgl_QueryCounter(GLuint, GLenum) {}
void mdbgl_GetQueryObjectiv(GLuint, GLenum, GLint* v) { if (v) *v = 0; }
void mdbgl_GetQueryObjectui64v(GLuint, GLenum, GLuint64* v) { if (v) *v = 0; }

#endif // __EMSCRIPTEN__
