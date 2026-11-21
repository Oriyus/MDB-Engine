#pragma once
// The web build's stand-in for glad (only on the include path of Emscripten builds; desktop builds use vendor/glad).
//
// The engine is written against desktop OpenGL 4.5 with direct state access (glCreate*, glNamed*, glTexture*,
// glProgramUniform*). A browser has WebGL2 (= OpenGL ES 3.0): no DSA, no storage buffers, no compute. This header
// gives the engine the calls it uses, implemented in Engine/src/Platform/WebGL.cpp on top of WebGL2:
//   - DSA calls bind the object on the side (a scratch texture unit, the copy-write buffer target, the draw
//     framebuffer), do the ES call, and put the binding back. The bindings are tracked here, so nothing is queried.
//   - glProgramUniform* switch the program lazily; draws use the program the engine last asked for.
//   - Storage buffers (every glCreateBuffers buffer that Buffer.cpp doesn't mark as vertex / index data) live as a
//     CPU copy mirrored into an RGBA32UI texture, 1024 texels (16 KB) a row, bound on a fixed texture unit per
//     binding point (WebStorageUnit). Shaders read them with texelFetch (EngineAssets/Shaders/web_storage.glsl).
//   - glCopyImageSubData is a framebuffer blit; glColorMaski / glBlendFunci use OES_draw_buffers_indexed.
//   - What WebGL2 can't do (compute, polygon mode, texture views, timer queries, reading textures back) does
//     nothing; the renderer leaves those features off on the web.
// Core ES 3.0 calls the engine makes (glClear, glViewport, ...) go straight to WebGL; the few whose state is
// tracked (glBindFramebuffer, glBindVertexArray, glUseProgram, draws, glEnable / glDisable) are redirected.

#include <GLES3/gl3.h>
#include <cstddef>
#include <cstdint>

#ifndef MDB_WEB_GL
#define MDB_WEB_GL 1
#endif

// ---- desktop-only enums the engine names (values from the GL 4.6 headers)
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#define GL_COMPUTE_SHADER                 0x91B9
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_TEXTURE_BORDER_COLOR           0x1004
#define GL_TEXTURE_CUBE_MAP_SEAMLESS      0x884F
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#define GL_LINE                           0x1B01
#define GL_FILL                           0x1B02
#define GL_POLYGON_OFFSET_LINE            0x2A02
#define GL_TIMESTAMP                      0x8E28
#define GL_DYNAMIC_STORAGE_BIT            0x0100
#define GL_CLIENT_STORAGE_BIT             0x0200
#define GL_MAP_PERSISTENT_BIT             0x0040
#define GL_MAP_COHERENT_BIT               0x0080
#define GL_SHADER_STORAGE_BARRIER_BIT     0x00002000
#define GL_BUFFER_UPDATE_BARRIER_BIT      0x00000200
#define GL_TEXTURE_SWIZZLE_RGBA           0x8E46
#define GL_TEXTURE_MAX_ANISOTROPY         0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY     0x84FF

typedef void* (*GLADloadproc)(const char* name);
inline int gladLoadGLLoader(GLADloadproc) { return 1; }

// ---- marking real (non-storage) buffers: Buffer.cpp calls this right after glCreateBuffers
void mdbgl_MarkBuffer(GLuint buffer, GLenum target);   // GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER
// The texture unit a storage buffer binding point is read from (-1: not available on the web)
int mdbgl_StorageUnit(GLuint binding);

// ---- creation
void mdbgl_CreateTextures(GLenum target, GLsizei n, GLuint* ids);
void mdbgl_CreateBuffers(GLsizei n, GLuint* ids);
void mdbgl_CreateFramebuffers(GLsizei n, GLuint* ids);
void mdbgl_CreateVertexArrays(GLsizei n, GLuint* ids);
void mdbgl_CreateSamplers(GLsizei n, GLuint* ids);
void mdbgl_DeleteTextures(GLsizei n, const GLuint* ids);
void mdbgl_DeleteBuffers(GLsizei n, const GLuint* ids);
void mdbgl_DeleteVertexArrays(GLsizei n, const GLuint* ids);
void mdbgl_DeleteFramebuffers(GLsizei n, const GLuint* ids);

// ---- textures
void mdbgl_TextureStorage2D(GLuint tex, GLsizei levels, GLenum format, GLsizei w, GLsizei h);
void mdbgl_TextureStorage3D(GLuint tex, GLsizei levels, GLenum format, GLsizei w, GLsizei h, GLsizei d);
void mdbgl_TextureSubImage2D(GLuint tex, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels);
void mdbgl_TextureSubImage3D(GLuint tex, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void* pixels);
void mdbgl_TextureParameteri(GLuint tex, GLenum pname, GLint v);
void mdbgl_TextureParameterf(GLuint tex, GLenum pname, GLfloat v);
void mdbgl_TextureParameterfv(GLuint tex, GLenum pname, const GLfloat* v);
void mdbgl_TextureParameteriv(GLuint tex, GLenum pname, const GLint* v);
void mdbgl_GenerateTextureMipmap(GLuint tex);
void mdbgl_BindTextureUnit(GLuint unit, GLuint tex);
void mdbgl_TextureView(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
void mdbgl_ClearTexImage(GLuint tex, GLint level, GLenum format, GLenum type, const void* data);
void mdbgl_GetTextureImage(GLuint tex, GLint level, GLenum format, GLenum type, GLsizei size, void* pixels);
void mdbgl_CopyImageSubData(GLuint src, GLenum srcTarget, GLint srcLevel, GLint sx, GLint sy, GLint sz,
                            GLuint dst, GLenum dstTarget, GLint dstLevel, GLint dx, GLint dy, GLint dz,
                            GLsizei w, GLsizei h, GLsizei d);

// ---- buffers
void mdbgl_NamedBufferData(GLuint buf, GLsizeiptr size, const void* data, GLenum usage);
void mdbgl_NamedBufferStorage(GLuint buf, GLsizeiptr size, const void* data, GLbitfield flags);
void mdbgl_NamedBufferSubData(GLuint buf, GLintptr offset, GLsizeiptr size, const void* data);
void mdbgl_ClearNamedBufferData(GLuint buf, GLenum internalformat, GLenum format, GLenum type, const void* data);
void mdbgl_CopyNamedBufferSubData(GLuint src, GLuint dst, GLintptr so, GLintptr dO, GLsizeiptr size);
void* mdbgl_MapNamedBufferRange(GLuint buf, GLintptr offset, GLsizeiptr length, GLbitfield access);
GLboolean mdbgl_UnmapNamedBuffer(GLuint buf);
void mdbgl_BindBufferBase(GLenum target, GLuint index, GLuint buffer);

// ---- framebuffers
void mdbgl_BindFramebuffer(GLenum target, GLuint fb);
void mdbgl_NamedFramebufferTexture(GLuint fb, GLenum attachment, GLuint tex, GLint level);
void mdbgl_NamedFramebufferTextureLayer(GLuint fb, GLenum attachment, GLuint tex, GLint level, GLint layer);
void mdbgl_NamedFramebufferDrawBuffers(GLuint fb, GLsizei n, const GLenum* bufs);
void mdbgl_NamedFramebufferDrawBuffer(GLuint fb, GLenum buf);
void mdbgl_NamedFramebufferReadBuffer(GLuint fb, GLenum buf);
GLenum mdbgl_CheckNamedFramebufferStatus(GLuint fb, GLenum target);
void mdbgl_BlitNamedFramebuffer(GLuint read, GLuint draw, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);
void mdbgl_ClearNamedFramebufferfv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLfloat* v);
void mdbgl_ClearNamedFramebufferiv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLint* v);
void mdbgl_ClearNamedFramebufferuiv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLuint* v);
void mdbgl_ClearNamedFramebufferfi(GLuint fb, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);

// ---- vertex arrays
void mdbgl_BindVertexArray(GLuint vao);
void mdbgl_VertexArrayVertexBuffer(GLuint vao, GLuint binding, GLuint buffer, GLintptr offset, GLsizei stride);
void mdbgl_EnableVertexArrayAttrib(GLuint vao, GLuint attrib);
void mdbgl_VertexArrayAttribFormat(GLuint vao, GLuint attrib, GLint size, GLenum type, GLboolean norm, GLuint offset);
void mdbgl_VertexArrayAttribIFormat(GLuint vao, GLuint attrib, GLint size, GLenum type, GLuint offset);
void mdbgl_VertexArrayAttribBinding(GLuint vao, GLuint attrib, GLuint binding);
void mdbgl_VertexArrayElementBuffer(GLuint vao, GLuint buffer);

// ---- programs and uniforms
void mdbgl_UseProgram(GLuint program);
void mdbgl_ProgramUniform1i(GLuint p, GLint loc, GLint v);
void mdbgl_ProgramUniform1f(GLuint p, GLint loc, GLfloat v);
void mdbgl_ProgramUniform2fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v);
void mdbgl_ProgramUniform3fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v);
void mdbgl_ProgramUniform4fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v);
void mdbgl_ProgramUniform1fv(GLuint p, GLint loc, GLsizei n, const GLfloat* v);
void mdbgl_ProgramUniform1iv(GLuint p, GLint loc, GLsizei n, const GLint* v);
void mdbgl_ProgramUniform4iv(GLuint p, GLint loc, GLsizei n, const GLint* v);
void mdbgl_ProgramUniformMatrix4fv(GLuint p, GLint loc, GLsizei n, GLboolean transpose, const GLfloat* v);

// ---- draws (sync the program first) and state
void mdbgl_DrawArrays(GLenum mode, GLint first, GLsizei count);
void mdbgl_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances);
void mdbgl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void mdbgl_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances);
void mdbgl_Enable(GLenum cap);
void mdbgl_Disable(GLenum cap);
GLboolean mdbgl_IsEnabled(GLenum cap);
void mdbgl_GetIntegerv(GLenum pname, GLint* v);
void mdbgl_ColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void mdbgl_BlendFunci(GLuint buf, GLenum src, GLenum dst);
void mdbgl_PolygonMode(GLenum face, GLenum mode);

// ---- not available in WebGL2 (do nothing)
void mdbgl_DispatchCompute(GLuint x, GLuint y, GLuint z);
void mdbgl_MemoryBarrier(GLbitfield barriers);
void mdbgl_QueryCounter(GLuint id, GLenum target);
void mdbgl_GetQueryObjectiv(GLuint id, GLenum pname, GLint* v);
void mdbgl_GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* v);

#define glCreateTextures              mdbgl_CreateTextures
#define glCreateBuffers               mdbgl_CreateBuffers
#define glCreateFramebuffers          mdbgl_CreateFramebuffers
#define glCreateVertexArrays          mdbgl_CreateVertexArrays
#define glCreateSamplers              mdbgl_CreateSamplers
#define glDeleteTextures              mdbgl_DeleteTextures
#define glDeleteBuffers               mdbgl_DeleteBuffers
#define glDeleteVertexArrays          mdbgl_DeleteVertexArrays
#define glDeleteFramebuffers          mdbgl_DeleteFramebuffers
#define glTextureStorage2D            mdbgl_TextureStorage2D
#define glTextureStorage3D            mdbgl_TextureStorage3D
#define glTextureSubImage2D           mdbgl_TextureSubImage2D
#define glTextureSubImage3D           mdbgl_TextureSubImage3D
#define glTextureParameteri           mdbgl_TextureParameteri
#define glTextureParameterf           mdbgl_TextureParameterf
#define glTextureParameterfv          mdbgl_TextureParameterfv
#define glTextureParameteriv          mdbgl_TextureParameteriv
#define glGenerateTextureMipmap       mdbgl_GenerateTextureMipmap
#define glBindTextureUnit             mdbgl_BindTextureUnit
#define glTextureView                 mdbgl_TextureView
#define glClearTexImage               mdbgl_ClearTexImage
#define glGetTextureImage             mdbgl_GetTextureImage
#define glCopyImageSubData            mdbgl_CopyImageSubData
#define glNamedBufferData             mdbgl_NamedBufferData
#define glNamedBufferStorage          mdbgl_NamedBufferStorage
#define glNamedBufferSubData          mdbgl_NamedBufferSubData
#define glClearNamedBufferData        mdbgl_ClearNamedBufferData
#define glCopyNamedBufferSubData      mdbgl_CopyNamedBufferSubData
#define glMapNamedBufferRange         mdbgl_MapNamedBufferRange
#define glUnmapNamedBuffer            mdbgl_UnmapNamedBuffer
#define glBindBufferBase              mdbgl_BindBufferBase
#define glBindFramebuffer             mdbgl_BindFramebuffer
#define glNamedFramebufferTexture     mdbgl_NamedFramebufferTexture
#define glNamedFramebufferTextureLayer mdbgl_NamedFramebufferTextureLayer
#define glNamedFramebufferDrawBuffers mdbgl_NamedFramebufferDrawBuffers
#define glNamedFramebufferDrawBuffer  mdbgl_NamedFramebufferDrawBuffer
#define glNamedFramebufferReadBuffer  mdbgl_NamedFramebufferReadBuffer
#define glCheckNamedFramebufferStatus mdbgl_CheckNamedFramebufferStatus
#define glBlitNamedFramebuffer        mdbgl_BlitNamedFramebuffer
#define glClearNamedFramebufferfv     mdbgl_ClearNamedFramebufferfv
#define glClearNamedFramebufferiv     mdbgl_ClearNamedFramebufferiv
#define glClearNamedFramebufferuiv    mdbgl_ClearNamedFramebufferuiv
#define glClearNamedFramebufferfi     mdbgl_ClearNamedFramebufferfi
#define glBindVertexArray             mdbgl_BindVertexArray
#define glVertexArrayVertexBuffer     mdbgl_VertexArrayVertexBuffer
#define glEnableVertexArrayAttrib     mdbgl_EnableVertexArrayAttrib
#define glVertexArrayAttribFormat     mdbgl_VertexArrayAttribFormat
#define glVertexArrayAttribIFormat    mdbgl_VertexArrayAttribIFormat
#define glVertexArrayAttribBinding    mdbgl_VertexArrayAttribBinding
#define glVertexArrayElementBuffer    mdbgl_VertexArrayElementBuffer
#define glUseProgram                  mdbgl_UseProgram
#define glProgramUniform1i            mdbgl_ProgramUniform1i
#define glProgramUniform1f            mdbgl_ProgramUniform1f
#define glProgramUniform2fv           mdbgl_ProgramUniform2fv
#define glProgramUniform3fv           mdbgl_ProgramUniform3fv
#define glProgramUniform4fv           mdbgl_ProgramUniform4fv
#define glProgramUniform1fv           mdbgl_ProgramUniform1fv
#define glProgramUniform1iv           mdbgl_ProgramUniform1iv
#define glProgramUniform4iv           mdbgl_ProgramUniform4iv
#define glProgramUniformMatrix4fv     mdbgl_ProgramUniformMatrix4fv
#define glDrawArrays                  mdbgl_DrawArrays
#define glDrawArraysInstanced         mdbgl_DrawArraysInstanced
#define glDrawElements                mdbgl_DrawElements
#define glDrawElementsInstanced       mdbgl_DrawElementsInstanced
#define glEnable                      mdbgl_Enable
#define glDisable                     mdbgl_Disable
#define glIsEnabled                   mdbgl_IsEnabled
#define glGetIntegerv                 mdbgl_GetIntegerv
#define glColorMaski                  mdbgl_ColorMaski
#define glBlendFunci                  mdbgl_BlendFunci
#define glPolygonMode                 mdbgl_PolygonMode
#define glDispatchCompute             mdbgl_DispatchCompute
#define glMemoryBarrier               mdbgl_MemoryBarrier
#define glQueryCounter                mdbgl_QueryCounter
#define glGetQueryObjectiv            mdbgl_GetQueryObjectiv
#define glGetQueryObjectui64v         mdbgl_GetQueryObjectui64v
