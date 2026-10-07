// GfxRenderingAPI for the Zune HD: sm64ex's GLES2 backend (gfx_opengl.c) ported to the ZDKGL
// wrappers. gfx_opengl.c is part of the Fast3D renderer, Copyright (c) 2020 Emill, MaikelChan,
// whose terms cover this file too: see section 2 of LICENSE. Differences from upstream:
//  - programs come from precompiled AR20 binaries (zune_shaders.h), never from GLSL source;
//  - the game sees a 480x272 landscape target; positions, viewports and scissors are rotated
//    onto the portrait 272x480 framebuffer;
//  - no state queries (ZDKGL_glGetIntegerv returns nothing), so all GL state is shadowed here
//    and re-applied after every ZDKGL_BeginDraw.
#include <stdlib.h>
#include <string.h>
extern "C" {
#include <PR/gbi.h>
#include "pc/gfx/gfx_cc.h"
#include "pc/gfx/gfx_rendering_api.h"
#include "pc/platform.h"
}
#include "zune_gl.h"
#include "zune_overlay.h"
#include "zune_platform.h"
#include "zune_settings.h"
#include "zune_shaders.h"

struct ShaderProgram {
    uint32_t shader_id;
    GLuint program;
    bool missing;                 // no precompiled binary: skipped and logged once
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;           // 32-bit slots per vertex in gfx_pc's batch (the stride)
    GLint attrib_locations[7];
    uint8_t attrib_sizes[7];      // GL component counts
    uint8_t attrib_bytes;         // bit i: attribute i is a normalized RGBA8 colour in one slot
    uint8_t num_attribs;
};

static ShaderProgram programs[64];
static int programCount;
static ShaderProgram *current;
static GLuint *textures;
static int textureCount, textureCapacity;
static int currentTile;
static GLuint boundTextures[2];

// Shadowed state, re-applied after each BeginDraw.
static bool inFrame;
static bool depthTest, depthMask = true, decal, blend;
static int viewport[4] = {0, 0, ZUNE_VIEW_W, ZUNE_VIEW_H};
static int scissor[4] = {0, 0, ZUNE_VIEW_W, ZUNE_VIEW_H};

ZuneRenderStats zuneRenderStats;
bool zuneRotateClockwise = false;
bool zuneShowTouchOverlay = ZUNE_TOUCH_BUTTONS != ZUNE_BUTTONS_HIDDEN;

static void SetCap(GLenum cap, bool on)
{
    if (on) ZDKGL_glEnable(cap); else ZDKGL_glDisable(cap);
}

static void ApplyViewport()
{
    int r[4];
    ZuneRotateRect(viewport[0], viewport[1], viewport[2], viewport[3], zuneRotateClockwise, r);
    ZDKGL_glViewport(r[0], r[1], r[2], r[3]);
}

static void ApplyScissor()
{
    int r[4];
    ZuneRotateRect(scissor[0], scissor[1], scissor[2], scissor[3], zuneRotateClockwise, r);
    ZDKGL_glScissor(r[0], r[1], r[2], r[3]);
}

// What the driver has been told about buffers, attribute arrays and textures, so program and
// texture switches only issue real changes: every GL call is a costly trip through
// compclient (~38% of render time was in system code). Forgotten at each frame start, since
// nothing guarantees state survives ZDKGL_BeginDraw/EndDraw.
enum { CACHED_LOCATIONS = 8, UNKNOWN = -1 };
static const GLuint UNKNOWN_BUFFER = 0xFFFFFFFFu;
struct AttribCache { int enabled; GLuint buffer; GLint size; GLsizei stride; size_t offset; bool bytes; };
static AttribCache attribCache[CACHED_LOCATIONS];
static GLuint boundBuffer;
static int activeUnit;
static GLuint unitTexture[2];
static bool unitKnown[2];

static void ForgetDriverState()
{
    for (int i = 0; i < CACHED_LOCATIONS; ++i) {
        attribCache[i].enabled = UNKNOWN;
        attribCache[i].size = 0;
    }
    boundBuffer = UNKNOWN_BUFFER;   // not 0: client arrays need a real glBindBuffer(0) after the overlay's VBO
    activeUnit = UNKNOWN;
    unitKnown[0] = unitKnown[1] = false;
}

static void BindBuffer(GLuint buffer)
{
    if (buffer == boundBuffer) return;
    ZDKGL_glBindBuffer(GL_ARRAY_BUFFER, buffer);
    boundBuffer = buffer;
}

static void EnableArray(GLint location, bool enable)
{
    if (location < 0) return;
    if (location < CACHED_LOCATIONS) {
        if (attribCache[location].enabled == (int)enable) return;
        attribCache[location].enabled = enable;
    }
    if (enable) ZDKGL_glEnableVertexAttribArray(location);
    else ZDKGL_glDisableVertexAttribArray(location);
    zuneRenderStats.stateCalls++;
}

// Float array (or normalized RGBA8 when `bytes`) at `pointer`: an offset into the bound
// buffer, or a client address when no buffer is bound.
static void ArrayPointer(GLint location, GLint size, GLsizei stride, const char *pointer, bool bytes)
{
    if (location < 0) return;
    size_t offset = (size_t)pointer;
    if (location < CACHED_LOCATIONS) {
        AttribCache &c = attribCache[location];
        if (c.size == size && c.stride == stride && c.offset == offset && c.buffer == boundBuffer && c.bytes == bytes) return;
        c.size = size; c.stride = stride; c.offset = offset; c.buffer = boundBuffer; c.bytes = bytes;
    }
    if (bytes) ZDKGL_glVertexAttribPointer(location, size, GL_UNSIGNED_BYTE, GL_TRUE, stride, pointer);
    else ZDKGL_glVertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, pointer);
    zuneRenderStats.stateCalls++;
}

// Enables exactly the program's arrays (so no stale array outlives its program) and points
// them at interleaved 32-bit slots starting at `base` (NULL: the start of the bound buffer).
// Attributes flagged in `byteMask` are RGBA8 colours taking one slot; the rest are floats.
static void UseArrays(const GLint *locations, const uint8_t *sizes, unsigned byteMask, int count, GLsizei stride,
                      const char *base)
{
    bool wanted[CACHED_LOCATIONS] = {false};
    for (int i = 0; i < count; ++i)
        if (locations[i] >= 0 && locations[i] < CACHED_LOCATIONS) wanted[locations[i]] = true;
    for (int l = 0; l < CACHED_LOCATIONS; ++l)
        if (!wanted[l] && attribCache[l].enabled != 0) EnableArray(l, false);
    size_t position = 0;
    for (int i = 0; i < count; ++i) {
        bool bytes = (byteMask >> i) & 1;
        EnableArray(locations[i], true);
        ArrayPointer(locations[i], sizes[i], stride, base + position * sizeof(float), bytes);
        position += bytes ? 1 : sizes[i];
    }
}

// Game draws use client-side arrays over gfx_pc's static vertex batch: on the device an
// upload (glBufferData) + draw costs ~189 us end to end against ~66 us for a client-array
// draw (measured), and the batch address never changes, so after a program
// switch the pointers are usually cache hits and a draw is a single glDrawArrays.
static void BindAttributes(const ShaderProgram *prg, const float *batch)
{
    BindBuffer(0);
    UseArrays(prg->attrib_locations, prg->attrib_sizes, prg->attrib_bytes, prg->num_attribs,
              prg->num_floats * sizeof(float), (const char *)batch);
}

static void BindTextureUnit(int tile, GLuint texture)
{
    if (unitKnown[tile] && unitTexture[tile] == texture) return;
    if (activeUnit != tile) {
        ZDKGL_glActiveTexture(GL_TEXTURE0 + tile);
        activeUnit = tile;
    }
    ZDKGL_glBindTexture(GL_TEXTURE_2D, texture);
    unitTexture[tile] = texture;
    unitKnown[tile] = true;
    zuneRenderStats.stateCalls++;
}

// glTexParameteri applies to the active unit's texture.
static void ActivateUnit(int tile)
{
    if (activeUnit == tile) return;
    ZDKGL_glActiveTexture(GL_TEXTURE0 + tile);
    activeUnit = tile;
}

static void ApplyState()
{
    ForgetDriverState();
    BindBuffer(0);
    ZDKGL_glDepthFunc(GL_LEQUAL);
    SetCap(GL_DEPTH_TEST, depthTest);
    ZDKGL_glDepthMask(depthMask ? GL_TRUE : GL_FALSE);
    SetCap(GL_BLEND, blend);
    if (decal) ZDKGL_glPolygonOffset(-2, -2);
    SetCap(GL_POLYGON_OFFSET_FILL, decal);
    ApplyViewport();
    ApplyScissor();
    SetCap(GL_SCISSOR_TEST, true);
    for (int tile = 1; tile >= 0; --tile) BindTextureUnit(tile, boundTextures[tile]);
    if (current && !current->missing) ZDKGL_glUseProgram(current->program);   // arrays: at the next draw
}

// GL calls are only issued inside ZDKGL_BeginDraw/EndDraw (as all working apps do); entry
// points that run outside a frame, such as gfx_init's shader precreation, open one.
static void EnsureFrame()
{
    if (inFrame) return;
    ZDKGL_BeginDraw();
    inFrame = true;
    ApplyState();
}

static bool zune_z_is_from_0_to_1(void) { return false; }

// Arrays are switched in load_shader (UseArrays disables what the next program does not use).
static void zune_unload_shader(ShaderProgram *old)
{
    if (old == current) current = NULL;
}

static void zune_load_shader(ShaderProgram *prg)
{
    current = prg;
    if (prg->missing) return;
    EnsureFrame();
    ZDKGL_glUseProgram(prg->program);
    zuneRenderStats.programSwitches++;
}

static const ZuneShaderBinary *FindBinary(uint32_t id)
{
    for (unsigned i = 0; i < sizeof(zune_shader_binaries) / sizeof(zune_shader_binaries[0]); ++i)
        if (zune_shader_binaries[i].id == id) return &zune_shader_binaries[i];
    return NULL;
}

static ShaderProgram *zune_create_and_load_new_shader(uint32_t shader_id)
{
    if (programCount == (int)(sizeof(programs) / sizeof(programs[0]))) sys_fatal("shader pool full (0x%08x)", shader_id);
    ShaderProgram *prg = &programs[programCount++];
    memset(prg, 0, sizeof(*prg));
    prg->shader_id = shader_id;
    struct CCFeatures cc;
    gfx_cc_get_features(shader_id, &cc);
    // Vertex layout produced by gfx_pc for this shader: position, [uv] floats, then [fog] and
    // the inputs as one RGBA8 slot each (patch 0011).
    prg->num_inputs = (uint8_t)cc.num_inputs;
    prg->used_textures[0] = cc.used_textures[0];
    prg->used_textures[1] = cc.used_textures[1];
    static const char *const inputNames[4] = {"aInput1", "aInput2", "aInput3", "aInput4"};
    const char *names[7];
    int count = 0;
    names[count] = "aVtxPos"; prg->attrib_sizes[count++] = 4;
    if (cc.used_textures[0] || cc.used_textures[1]) { names[count] = "aTexCoord"; prg->attrib_sizes[count++] = 2; }
    if (cc.opt_fog) { prg->attrib_bytes |= 1 << count; names[count] = "aFog"; prg->attrib_sizes[count++] = 4; }
    for (int i = 0; i < cc.num_inputs; ++i) {
        prg->attrib_bytes |= 1 << count;
        names[count] = inputNames[i];
        prg->attrib_sizes[count++] = cc.opt_alpha ? 4 : 3;
    }
    prg->num_attribs = (uint8_t)count;
    for (int i = 0; i < count; ++i)
        prg->num_floats = (uint8_t)(prg->num_floats + (((prg->attrib_bytes >> i) & 1) ? 1 : prg->attrib_sizes[i]));

    const ZuneShaderBinary *binary = FindBinary(shader_id);
    if (!binary) {
        prg->missing = true;
        ZuneLog("shader 0x%08x has no precompiled binary; its triangles are skipped", shader_id);
        current = prg;
        return prg;
    }
    ZuneStep("shader binary load");
    EnsureFrame();
    GLuint vs = ZDKGL_glCreateShader(GL_VERTEX_SHADER), fs = ZDKGL_glCreateShader(GL_FRAGMENT_SHADER);
    prg->program = ZDKGL_glCreateProgram();
    ZDKGL_glAttachShader(prg->program, vs);
    ZDKGL_glAttachShader(prg->program, fs);
    ZDKGL_glShaderBinary(1, &vs, ZUNE_GL_NVIDIA_PLATFORM_BINARY_NV, binary->vs, (GLsizei)binary->vs_size);
    ZDKGL_glShaderBinary(1, &fs, ZUNE_GL_NVIDIA_PLATFORM_BINARY_NV, binary->fs, (GLsizei)binary->fs_size);
    ZDKGL_glLinkProgram(prg->program);
    GLint linked = 0;
    ZDKGL_glGetProgramiv(prg->program, GL_LINK_STATUS, &linked);   // verified to work on the device
    if (!linked) {
        ZuneLog("shader 0x%08x failed to link (GL error 0x%x)", shader_id, ZDKGL_glGetError());
        prg->missing = true;
        current = prg;
        return prg;
    }
    for (int i = 0; i < count; ++i) prg->attrib_locations[i] = ZDKGL_glGetAttribLocation(prg->program, names[i]);
    zune_load_shader(prg);
    if (cc.used_textures[0]) ZDKGL_glUniform1i(ZDKGL_glGetUniformLocation(prg->program, "uTex0"), 0);
    if (cc.used_textures[1]) ZDKGL_glUniform1i(ZDKGL_glGetUniformLocation(prg->program, "uTex1"), 1);
    ZuneLog("shader 0x%08x ready: program %u, %d attributes", shader_id, prg->program, count);
    return prg;
}

static ShaderProgram *zune_lookup_shader(uint32_t shader_id)
{
    for (int i = 0; i < programCount; ++i)
        if (programs[i].shader_id == shader_id) return &programs[i];
    return NULL;
}

static void zune_shader_get_info(ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2])
{
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t zune_new_texture(void)
{
    if (textureCount == textureCapacity) {
        textureCapacity += 512;
        textures = (GLuint *)realloc(textures, sizeof(GLuint) * textureCapacity);
        if (!textures) sys_fatal("out of memory allocating texture cache");
    }
    EnsureFrame();
    ZDKGL_glGenTextures(1, &textures[textureCount]);
    return (uint32_t)textureCount++;
}

static void zune_select_texture(int tile, uint32_t texture_id)
{
    EnsureFrame();
    currentTile = tile;
    boundTextures[tile] = textures[texture_id];
    BindTextureUnit(tile, boundTextures[tile]);
}

static void zune_upload_texture(const uint8_t *rgba32_buf, int width, int height)
{
    EnsureFrame();
    ActivateUnit(currentTile);   // the texture select_texture just bound
    ZDKGL_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    zuneRenderStats.textureUploads++;
}

static GLint WrapMode(uint32_t value)
{
    if (value & G_TX_CLAMP) return GL_CLAMP_TO_EDGE;
    return (value & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void zune_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt)
{
    EnsureFrame();
    GLint filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    ActivateUnit(tile);
    zuneRenderStats.samplerSets++;
    ZDKGL_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    ZDKGL_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    ZDKGL_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapMode(cms));
    ZDKGL_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapMode(cmt));
    currentTile = tile;
}

static void zune_set_depth_test(bool on) { depthTest = on; EnsureFrame(); SetCap(GL_DEPTH_TEST, on); }
static void zune_set_depth_mask(bool on) { depthMask = on; EnsureFrame(); ZDKGL_glDepthMask(on ? GL_TRUE : GL_FALSE); }

static void zune_set_zmode_decal(bool on)
{
    decal = on;
    EnsureFrame();
    ZDKGL_glPolygonOffset(on ? -2.0f : 0.0f, on ? -2.0f : 0.0f);
    SetCap(GL_POLYGON_OFFSET_FILL, on);
}

static void zune_set_viewport(int x, int y, int width, int height)
{
    viewport[0] = x; viewport[1] = y; viewport[2] = width; viewport[3] = height;
    EnsureFrame();
    ApplyViewport();
}

static void zune_set_scissor(int x, int y, int width, int height)
{
    scissor[0] = x; scissor[1] = y; scissor[2] = width; scissor[3] = height;
    EnsureFrame();
    ApplyScissor();
}

static void zune_set_use_alpha(bool on) { blend = on; EnsureFrame(); SetCap(GL_BLEND, on); }

static void zune_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!current || current->missing) return;
    EnsureFrame();
    // gfx_pc emits landscape clip coordinates; turn them onto the portrait panel.
    size_t stride = current->num_floats;
    for (size_t i = 0; i + 1 < buf_vbo_len; i += stride) ZuneRotateClip(&buf_vbo[i], &buf_vbo[i + 1], zuneRotateClockwise);
    BindAttributes(current, buf_vbo);
    ZDKGL_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(3 * buf_vbo_num_tris));
    zuneRenderStats.drawCalls++;
    zuneRenderStats.triangles += (int)buf_vbo_num_tris;
}

static void zune_init(void)
{
    EnsureFrame();
    ApplyState();
}

static void zune_on_resize(void) {}

static void zune_start_frame(void)
{
    EnsureFrame();
    ZDKGL_glDisable(GL_SCISSOR_TEST);
    ZDKGL_glDepthMask(GL_TRUE);   // must be set to clear the depth buffer
    ZDKGL_glClearColor(0, 0, 0, 1);
    ZDKGL_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ZDKGL_glDepthMask(depthMask ? GL_TRUE : GL_FALSE);
    ZDKGL_glEnable(GL_SCISSOR_TEST);
}

static void zune_end_frame(void) {}
static void zune_finish_render(void) {}
static void zune_shutdown(void) {}

// ---------------------------------------------------------------- touch overlay
// Buttons (zune_overlay.h) live in a static VBO built once: every button idle, then every
// button pressed, so a frame with nothing pressed is one draw. The stick is rebuilt each frame
// while held and drawn from a client array. The overlay program has blending compiled in.
// Style and opacity are build settings; with hidden buttons the overlay is never set up.
static const ZuneOverlayStyle overlayStyle = {ZUNE_TOUCH_BUTTONS, ZUNE_TOUCH_OPACITY / 100.0f};

static GLuint overlayProgram, overlayButtons;
static GLint overlayPos = -1, overlayColor = -1;
static bool overlayFailed;
static int buttonFirst[2][ZUNE_BUTTON_COUNT], buttonCount[2][ZUNE_BUTTON_COUNT];
enum { STICK_VERTICES = 3 * 4 * ZuneOverlayBuilder::SEGMENTS };
static ZuneOverlayVertex stickVertices[STICK_VERTICES];

static bool InitOverlay()
{
    if (overlayProgram || overlayFailed) return overlayProgram != 0;
    overlayFailed = true;
    GLuint vs = ZDKGL_glCreateShader(GL_VERTEX_SHADER), fs = ZDKGL_glCreateShader(GL_FRAGMENT_SHADER);
    GLuint program = ZDKGL_glCreateProgram();
    ZDKGL_glAttachShader(program, vs);
    ZDKGL_glAttachShader(program, fs);
    ZDKGL_glShaderBinary(1, &vs, ZUNE_GL_NVIDIA_PLATFORM_BINARY_NV, zune_overlay_vs, sizeof(zune_overlay_vs));
    ZDKGL_glShaderBinary(1, &fs, ZUNE_GL_NVIDIA_PLATFORM_BINARY_NV, zune_overlay_fs, sizeof(zune_overlay_fs));
    ZDKGL_glLinkProgram(program);
    GLint linked = 0;
    ZDKGL_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    overlayPos = ZDKGL_glGetAttribLocation(program, "aPos");
    overlayColor = ZDKGL_glGetAttribLocation(program, "aColor");
    if (!linked || overlayPos < 0 || overlayColor < 0) {
        ZuneLog("touch overlay program unusable (linked %d, aPos %d, aColor %d)", linked, overlayPos, overlayColor);
        return false;
    }
    static ZuneOverlayVertex vertices[2 * ZUNE_BUTTON_COUNT * 480];
    ZuneOverlayBuilder builder(vertices, sizeof(vertices) / sizeof(vertices[0]));
    for (int pressed = 0; pressed < 2; ++pressed)
        for (int i = 0; i < ZUNE_BUTTON_COUNT; ++i) {
            buttonFirst[pressed][i] = builder.Count();
            ZuneBuildButton(builder, ZUNE_BUTTONS[i], pressed != 0, overlayStyle);
            buttonCount[pressed][i] = builder.Count() - buttonFirst[pressed][i];
        }
    if (builder.Overflowed()) { ZuneLog("touch overlay geometry overflowed"); return false; }
    ZuneOverlayToClip(vertices, builder.Count(), zuneRotateClockwise);
    ZDKGL_glGenBuffers(1, &overlayButtons);
    BindBuffer(overlayButtons);
    ZDKGL_glBufferData(GL_ARRAY_BUFFER, builder.Count() * sizeof(ZuneOverlayVertex), vertices, GL_STATIC_DRAW);
    overlayProgram = program;
    overlayFailed = false;
    ZuneLog("touch overlay ready: %d button vertices, program %u", builder.Count(), program);
    return true;
}

// buffer 0: client array at `vertices`.
static void OverlayAttributes(GLuint buffer, const ZuneOverlayVertex *vertices)
{
    static const uint8_t sizes[2] = {2, 4};
    GLint locations[2] = {overlayPos, overlayColor};
    BindBuffer(buffer);
    UseArrays(locations, sizes, 0, 2, sizeof(ZuneOverlayVertex), (const char *)vertices);
}

static void DrawTouchOverlay()
{
    if (!InitOverlay()) return;
    ZDKGL_glDisable(GL_SCISSOR_TEST);
    ZDKGL_glDisable(GL_DEPTH_TEST);
    ZDKGL_glViewport(0, 0, ZUNE_PANEL_W, ZUNE_PANEL_H);
    ZDKGL_glUseProgram(overlayProgram);
    OverlayAttributes(overlayButtons, NULL);
    if (!zuneLastPad.buttons) {
        ZDKGL_glDrawArrays(GL_TRIANGLES, 0, buttonFirst[1][0]);   // the whole idle block
    } else {
        for (int i = 0; i < ZUNE_BUTTON_COUNT; ++i) {
            int pressed = (zuneLastPad.buttons & ZUNE_BUTTONS[i].bits) ? 1 : 0;
            ZDKGL_glDrawArrays(GL_TRIANGLES, buttonFirst[pressed][i], buttonCount[pressed][i]);
        }
    }
    if (zuneStickView.held) {
        ZuneOverlayBuilder builder(stickVertices, STICK_VERTICES);
        ZuneBuildStick(builder, zuneStickView.originX, zuneStickView.originY, zuneStickView.x, zuneStickView.y, overlayStyle);
        ZuneOverlayToClip(stickVertices, builder.Count(), zuneRotateClockwise);
        OverlayAttributes(0, stickVertices);
        ZDKGL_glDrawArrays(GL_TRIANGLES, 0, builder.Count());
    }
    // The next frame's EnsureFrame/ApplyState restores the game's program, arrays and state.
}

// Landscape rectangle (origin top-left) filled with a scissored clear.
static void FillRect(int x, int y, int w, int h, float r, float g, float b)
{
    int rect[4];
    ZuneRotateRect(x, ZUNE_VIEW_H - y - h, w, h, zuneRotateClockwise, rect);
    ZDKGL_glScissor(rect[0], rect[1], rect[2], rect[3]);
    ZDKGL_glClearColor(r, g, b, 1);
    ZDKGL_glClear(GL_COLOR_BUFFER_BIT);
}

void ZuneDrawProgress(float fraction, int state)
{
    if (inFrame) ZuneRenderPresent();
    ZDKGL_BeginDraw();
    ZDKGL_glDisable(GL_SCISSOR_TEST);
    ZDKGL_glViewport(0, 0, ZUNE_PANEL_W, ZUNE_PANEL_H);
    ZDKGL_glClearColor(0.04f, 0.04f, 0.07f, 1);
    ZDKGL_glClear(GL_COLOR_BUFFER_BIT);
    ZDKGL_glEnable(GL_SCISSOR_TEST);
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    const int x = 90, y = 128, w = 300, h = 16;
    FillRect(x - 2, y - 2, w + 4, h + 4, 0.5f, 0.5f, 0.55f);
    FillRect(x, y, w, h, 0.1f, 0.1f, 0.12f);
    float r = state < 0 ? 0.9f : state > 0 ? 0.2f : 0.25f, g = state < 0 ? 0.2f : state > 0 ? 0.8f : 0.45f;
    float b = state < 0 ? 0.2f : state > 0 ? 0.3f : 1.0f;
    int filled = state < 0 ? w : (int)(w * fraction);
    if (filled > 0) FillRect(x, y, filled, h, r, g, b);
    ZDKGL_EndDraw();
}

void ZuneRenderPresent()
{
    if (!inFrame) return;
    if (zuneShowTouchOverlay) DrawTouchOverlay();
    ZDKGL_EndDraw();   // the GL server's work for the frame, then the display wait
    inFrame = false;
}

extern "C" {
struct GfxRenderingAPI zune_rendering_api = {
    zune_z_is_from_0_to_1,
    zune_unload_shader,
    zune_load_shader,
    zune_create_and_load_new_shader,
    zune_lookup_shader,
    zune_shader_get_info,
    zune_new_texture,
    zune_select_texture,
    zune_upload_texture,
    zune_set_sampler_parameters,
    zune_set_depth_test,
    zune_set_depth_mask,
    zune_set_zmode_decal,
    zune_set_viewport,
    zune_set_scissor,
    zune_set_use_alpha,
    zune_draw_triangles,
    zune_init,
    zune_on_resize,
    zune_start_frame,
    zune_end_frame,
    zune_finish_render,
    zune_shutdown
};
}
