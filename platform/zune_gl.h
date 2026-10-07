#ifndef ZUNE_GL_H
#define ZUNE_GL_H
// ZDKGL wrappers exported by zdksystem.dll: the GLES2 path that works on the device.
// OpenZDK's zdkgl.h omits these declarations. Rules learned on hardware (docs/porting.md):
//  - never call compclient.dll's plain gl*/egl* exports: the first such call killed the app;
//  - ZDKGL_glGetIntegerv writes nothing (and glGetShaderiv writes garbage), so never query state;
//  - glFinish returns immediately; work is flushed at ZDKGL_EndDraw (or by glReadPixels);
//  - shaders come from glShaderBinary(GL_NVIDIA_PLATFORM_BINARY_NV) only.
#include <windows.h>
#include <GLES2/gl2.h>

#define ZUNE_GL_NVIDIA_PLATFORM_BINARY_NV 0x890B

extern "C" {
HRESULT WINAPI ZDKGL_Initialize();
HRESULT WINAPI ZDKGL_Cleanup();
HRESULT WINAPI ZDKGL_BeginDraw();
HRESULT WINAPI ZDKGL_EndDraw();
void WINAPI ZDKGL_glActiveTexture(GLenum texture);
void WINAPI ZDKGL_glAttachShader(GLuint program, GLuint shader);
void WINAPI ZDKGL_glBindBuffer(GLenum target, GLuint buffer);
void WINAPI ZDKGL_glBindTexture(GLenum target, GLuint texture);
void WINAPI ZDKGL_glBlendFunc(GLenum sfactor, GLenum dfactor);
void WINAPI ZDKGL_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void WINAPI ZDKGL_glClear(GLbitfield mask);
void WINAPI ZDKGL_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
GLuint WINAPI ZDKGL_glCreateProgram();
GLuint WINAPI ZDKGL_glCreateShader(GLenum type);
void WINAPI ZDKGL_glDepthFunc(GLenum func);
void WINAPI ZDKGL_glDepthMask(GLboolean flag);
void WINAPI ZDKGL_glDisable(GLenum cap);
void WINAPI ZDKGL_glDisableVertexAttribArray(GLuint index);
void WINAPI ZDKGL_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void WINAPI ZDKGL_glEnable(GLenum cap);
void WINAPI ZDKGL_glEnableVertexAttribArray(GLuint index);
void WINAPI ZDKGL_glGenBuffers(GLsizei n, GLuint *buffers);
void WINAPI ZDKGL_glGenTextures(GLsizei n, GLuint *textures);
GLint WINAPI ZDKGL_glGetAttribLocation(GLuint program, const GLchar *name);
GLenum WINAPI ZDKGL_glGetError();
void WINAPI ZDKGL_glGetProgramiv(GLuint program, GLenum pname, GLint *params);
GLint WINAPI ZDKGL_glGetUniformLocation(GLuint program, const GLchar *name);
void WINAPI ZDKGL_glLinkProgram(GLuint program);
void WINAPI ZDKGL_glPixelStorei(GLenum pname, GLint param);
void WINAPI ZDKGL_glPolygonOffset(GLfloat factor, GLfloat units);
void WINAPI ZDKGL_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void WINAPI ZDKGL_glShaderBinary(GLsizei count, const GLuint *shaders, GLenum format, const void *binary, GLsizei length);
void WINAPI ZDKGL_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                               GLint border, GLenum format, GLenum type, const void *pixels);
void WINAPI ZDKGL_glTexParameteri(GLenum target, GLenum pname, GLint param);
void WINAPI ZDKGL_glUniform1i(GLint location, GLint v0);
void WINAPI ZDKGL_glUseProgram(GLuint program);
void WINAPI ZDKGL_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                        const void *pointer);
void WINAPI ZDKGL_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
}
#endif
