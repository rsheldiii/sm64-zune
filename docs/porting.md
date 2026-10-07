# Porting notes

What the Zune HD needed, for anyone changing this port or writing other native code for
the device. Everything here was measured or observed on one Zune HD (firmware 4.5).

## The device

- ARMv6 with VFP (no NEON), Windows CE 6, 480x272 screen wired as a 272x480 portrait panel,
  an NVIDIA Tegra GPU with an OpenGL ES 2 driver.
- Applications are XNA games. Native code runs through OpenZDK: a small XNA launcher
  ([`launcher/`](../launcher/)) starts `nativeapp.exe`, which the package carries as content.
  The launcher depends on exactly how it is compiled. Built by Visual Studio 2008 it works;
  the same source built by a current C# compiler closed at once on the device. So
  `launcher/exploiter.exe` is kept as Visual Studio built it, and `tests/run.sh` checks it.
- A package can be written to the device over USB, and nothing can be read back. The Zune
  takes a new USB address each time it reconnects, which it does after every deployment.
- Wi-Fi is off while the Zune is on USB.

## Compiler

The only compiler that targets the Zune's CE 6 is Visual C++ 2008's (VC9): C89 with
Microsoft extensions, and C++98. sm64ex is C99 with GNU extensions.

- `compat/sm64_vc9.h` is included first in every file and supplies the small things:
  `inline`, `__attribute__`, static assertions, `M_PI`. It also defines the byte-order macros.
  Without them every `#if IS_BIG_ENDIAN` is silently true and the whole game builds big-endian.
- Patches 0001 to 0007 fix what a header cannot: declarations after statements, and two
  macros that Microsoft's preprocessor handles differently.
- The renderer core (`gfx_pc.c`, `gfx_cc.c`) and libultra glue use too much C99 to patch, so
  `platform/zune_wrap_*.cpp` compile them as C++ with C linkage (patch 0008 adds the casts).
- The Windows CE headers define `small` as a macro. Do not name anything `small`.
- Flags: `/O2 /fp:fast /QRarch6 /QRfpe- /GS-`. `/QRfpe-` makes float arithmetic VFP
  instructions; see the next section for what it does not cover.
- The compiler and linker run under Wine. The game they produce there is identical to the
  one Visual Studio 2008 builds on Windows, apart from the timestamp in the file header.
  `tools/fetch.py` takes the twelve files it needs from Microsoft's trial ISO with HTTP range
  requests; nothing is installed.
- Link order is fixed by `tools/units.txt`, so a build does not depend on the order in which
  a parallel `make` happened to compile sm64ex.

## Floating point

`coredll`, the system's C library, does all of `<math.h>` in software, because it also runs
on ARMs without an FPU. One `sin()` costs 13.7 µs. sm64 calls `sqrtf`, `sinf`, `cosf`,
`floorf` and `fabsf` constantly: in one level, 44% of game-logic time was inside them.

- `compat/zune_math.h` redirects the float functions the game uses: `sqrtf` is the VFP's own
  instruction (`platform/zune_vfp.asm`), `sinf` and `cosf` are polynomials evaluated in
  hardware double (`platform/zune_libm.c`, within one ulp; `tests/libm.c`), the rest are inline.
- The compiler's 64-bit-integer-to-double conversion is also a software call. The clock code
  converts by halves instead.
- Both threads run the VFP in flush-to-zero mode, as the N64 did.

## Graphics

- The driver cannot compile GLSL (`GL_SHADER_COMPILER` is 0). `tools/shaders.py` cuts sm64ex's
  shader generator out of `gfx_opengl.c`, runs it for the 26 shader IDs the game uses, and
  compiles the results with NVIDIA's `cgc` and `shaderfix`. The game loads them with
  `glShaderBinary(GL_NVIDIA_PLATFORM_BINARY_NV)`.
- The GPU has no fixed-function blender: for a binary shader, `glBlendFunc` and `GL_BLEND` do
  nothing. Blending is compiled into the fragment shader with a `cgc` pragma, which
  `tools/shaders.py` adds to the shaders sm64ex blends with.
- Use the `ZDKGL_gl*` functions that `zdksystem.dll` exports (`platform/zune_gl.h`; OpenZDK's
  header omits them). Calling the plain `gl*` exports of `compclient.dll` killed the app.
- GL calls go between `ZDKGL_BeginDraw` and `ZDKGL_EndDraw`. State queries return nothing, so
  `platform/zune_render.cpp` keeps its own copy of the GL state and applies it again after
  every `BeginDraw`.
- The framebuffer is portrait. The backend rotates positions, viewports and scissors.
- A draw from client-side arrays costs about 66 µs; uploading a buffer first makes it about
  189 µs. Draw calls and state changes are what to save. Patches 0010 to 0014 do that in
  sm64ex's renderer: fewer divides per vertex, colours as bytes, a batch kept across a
  redundant texture change, and the sky drawn from one texture instead of nine.

## Sound

- sm64's synthesis runs on its own thread above the game's priority, as on the N64, so slow
  frames slow the game and not the music. A lock keeps it out of the game's audio state.
- Output goes to CE's `waveOut`. `ZDKAudio` is the fallback; OpenZDK's header for it does not
  match the device, and `platform/zune_audio.cpp` records what the device code really takes.
- The sound banks are laid out again for a 32-bit little-endian machine (`tools/build.py`,
  step `sound`).
- Patch 0009 gives the mixer a fast path once a channel's volume has settled. It is exact:
  `tests/envmixer.c` compares 60,000 random calls with the original.
- The game's data is paged in from flash on first touch, which stalled the first notes of
  each sound. The sound data is read through once at startup.

## Input and pacing

- The touch panel is read by its own thread about every 13 ms, and `ZunePadLatch`
  (`platform/zune_touch.h`) makes sure a tap shorter than a 1/30 s game tick still reaches
  the game.
- `platform/zune_pacing.h` holds 30 ticks a second and decides when to skip drawing
  (`ZUNE_FRAME_SKIP`).

## Deployment

- zune-deploy can report success after an error. `./sm64zune deploy` goes by the three steps
  it must print, and retries.
- A transfer that is cut short leaves an app that closes at once. The deployment therefore
  runs in its own session, where Ctrl-C does not reach it.

## Finding faults

- `ZUNE_LOG=on` records start-up steps, a statistics line every five seconds and the details
  of a crash, and sends them when the game is left (`./sm64zune logs`).
- `ZUNE_PROFILE=on` adds a sampling profiler. `tools/profile.py` charges samples to
  functions with the build's linker map, kept in `build/maps/`.
- `tools/symbolize.py` names the addresses of a crash, from a log or from the message on
  the Zune's screen.
