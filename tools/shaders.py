"""sm64ex's shaders, compiled ahead of time for the Zune HD.

The Zune's driver cannot compile GLSL (GL_SHADER_COMPILER is 0), so every shader the game
will ask for is compiled here with NVIDIA's cgc and shaderfix into the Tegra's binary format
and embedded in the game as platform/zune_shaders.h, for glShaderBinary(0x890B).

Which shaders: the 26 IDs sm64ex's gfx_pc.c creates at startup ("used in the 120 star TAS"),
which cover the whole game, plus the touch overlay's own pair (shaders/).

Their GLSL comes from sm64ex's own generator: the function that writes it is cut out of
gfx_opengl.c, compiled for this machine with USE_GLES and run, so the text is exactly what
the game would have handed to the driver (with linear filtering, configFiltering 1).

The Tegra's AR20 pipeline has no fixed-function blender: blending is compiled into the
fragment program, and glBlendFunc/GL_BLEND do nothing for a binary shader. sm64ex blends
exactly when a shader ID has SHADER_OPT_ALPHA, so those fragment shaders get cgc's pragma for
SRC_ALPHA, ONE_MINUS_SRC_ALPHA blending.
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

import wine

SHADER_OPT_ALPHA = 1 << 24
BLEND_PRAGMA = '#pragma profilepragma blendoperation(gl_FragColor, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)'
# cgc records the source's Windows path in the binary; a fixed one keeps the output reproducible.
SOURCE_DIR = 'zune/shaders'


def shader_ids(sm64ex):
    source = (sm64ex / 'src/pc/gfx/gfx_pc.c').read_text()
    block = source[source.index('static uint32_t precomp_shaders[] = {'):]
    return [int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{8})', block[:block.index('};')])]


def cut(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin) + len(end)]


def generate_glsl(sm64ex, ids, out, work):
    """Writes sm64_<id>.vs and .fs for each ID, using the generator cut out of gfx_opengl.c."""
    source = (sm64ex / 'src/pc/gfx/gfx_opengl.c').read_text()
    signature = 'static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {'
    helpers = cut(source, 'static void append_str(', signature)
    helpers = helpers[:helpers.rindex('static struct ShaderProgram')]
    body = cut(source, signature, "fs_buf[fs_len] = '\\0';")
    body = body.replace(signature, 'static void generate(uint32_t shader_id, char *vs_out, char *fs_out) {', 1)
    (work / 'generator.c').write_text(f'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USE_GLES 1
#include "gfx_cc.h"
static unsigned int configFiltering = 1;
{helpers}
{body}
    strcpy(vs_out, vs_buf);
    strcpy(fs_out, fs_buf);
}}
int main(int argc, char **argv) {{
    static char vs[4096], fs[8192];
    for (int i = 2; i < argc; ++i) {{
        uint32_t id = (uint32_t)strtoul(argv[i], NULL, 16);
        char path[512];
        generate(id, vs, fs);
        snprintf(path, sizeof(path), "%s/sm64_%08x.vs", argv[1], id);
        FILE *f = fopen(path, "w"); fputs(vs, f); fclose(f);
        snprintf(path, sizeof(path), "%s/sm64_%08x.fs", argv[1], id);
        f = fopen(path, "w"); fputs(fs, f); fclose(f);
    }}
    return 0;
}}
''')
    subprocess.run(['gcc', '-std=gnu99', '-O1', '-w', '-I', str(sm64ex / 'src/pc/gfx'), str(work / 'generator.c'),
                    '-o', str(work / 'generator')], check=True)
    subprocess.run([str(work / 'generator'), str(out), *[f'{i:08x}' for i in ids]], check=True)


def compile_shader(source, tools, drive):
    """GLSL file -> AR20 binary (bytes), through cgc and shaderfix."""
    vertex = source.suffix == '.vs'
    shutil.copy(source, drive / SOURCE_DIR / source.name)
    intermediate = f'c:\\zune-out\\{source.name}.cgbin'
    binary = f'{source.stem}.{"nvbv" if vertex else "nvbf"}'
    code, text = wine.run(tools / 'cgc.exe', '-profile', 'ar20vp' if vertex else 'ar20fp', '-ogles', '-o', intermediate,
                          'c:\\' + SOURCE_DIR.replace('/', '\\') + '\\' + source.name, '-po', 'filename')
    if code == 0:
        code, more = wine.run(tools / 'shaderfix.exe', '-o', 'c:\\zune-out\\' + binary, intermediate)
        text += more
    result = drive / 'zune-out' / binary
    if code != 0 or not result.is_file() or not result.stat().st_size:
        raise SystemExit(f'Shader {source.name} did not compile:\n{text}')
    return result.read_bytes()


def array(symbol, data, note):
    lines = [f'/* {note}{len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()} */',
             f'static const unsigned char {symbol}[{len(data)}] = {{']
    lines += ['    ' + ','.join(f'0x{b:02x}' for b in data[offset:offset + 20]) + ','
              for offset in range(0, len(data), 20)]
    return lines + ['};']


def build(sm64ex, overlay_dir, tools, work, header):
    """Compiles every shader and writes `header`. `work` is a scratch directory."""
    ids = shader_ids(sm64ex)
    if work.exists():
        shutil.rmtree(work)
    glsl = work / 'glsl'
    glsl.mkdir(parents=True)
    generate_glsl(sm64ex, ids, glsl, work)
    for shader_id in ids:
        if shader_id & SHADER_OPT_ALPHA:
            fragment = glsl / f'sm64_{shader_id:08x}.fs'
            lines = fragment.read_text().split('\n')
            if lines[0] != '#version 100':
                raise SystemExit(f'{fragment.name}: expected "#version 100" first')
            fragment.write_text('\n'.join([lines[0], BLEND_PRAGMA] + lines[1:]))
    for overlay in sorted(overlay_dir.glob('zune_overlay.*')):
        shutil.copy(overlay, glsl / overlay.name)

    drive = wine.prepare()
    for directory in (SOURCE_DIR, 'zune-out'):
        shutil.rmtree(drive / directory, ignore_errors=True)
        (drive / directory).mkdir(parents=True)
    sources = sorted(glsl.glob('*.vs')) + sorted(glsl.glob('*.fs'))
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        compiled = dict(zip((s.name for s in sources), pool.map(lambda s: compile_shader(s, tools, drive), sources)))

    lines = ['/* Generated by tools/shaders.py; do not edit.',
             ' * sm64ex combiner shaders (configFiltering 1, GLES) compiled with NVIDIA cgc -profile',
             ' * ar20vp/ar20fp + shaderfix (Tegra 250 CE6 pack 5265393) for glShaderBinary(0x890B).',
             ' * Shaders with SHADER_OPT_ALPHA (0x01000000) have SRC_ALPHA/ONE_MINUS_SRC_ALPHA blending',
             ' * compiled in: AR20 blends in the fragment program, not via glBlendFunc. */',
             '#ifndef ZUNE_SHADERS_H', '#define ZUNE_SHADERS_H', '']
    entries = []
    for shader_id in ids:
        names = []
        for kind in ('vs', 'fs'):
            names.append(f'zune_shader_{shader_id:08x}_{kind[0]}')
            lines += array(names[-1], compiled[f'sm64_{shader_id:08x}.{kind}'], '')
        entries.append(f'    {{0x{shader_id:08x}u, {names[0]}, sizeof({names[0]}), {names[1]}, sizeof({names[1]})}},')
    for kind in ('vs', 'fs'):
        lines += array(f'zune_overlay_{kind}', compiled[f'zune_overlay.{kind}'], f'touch overlay {kind}: ')
    lines += ['', 'struct ZuneShaderBinary {', '    unsigned int id;', '    const unsigned char *vs;',
              '    unsigned int vs_size;', '    const unsigned char *fs;', '    unsigned int fs_size;', '};', '',
              'static const struct ZuneShaderBinary zune_shader_binaries[] = {', *entries, '};', '', '#endif', '']
    header.write_text('\n'.join(lines))
    return len(ids)
