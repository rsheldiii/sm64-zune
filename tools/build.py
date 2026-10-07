#!/usr/bin/env python3
"""Build the game from a ROM: /rom plus this repository -> build/package.

Runs inside the build container (./sm64zune build), with the repository at /repo (read-only)
and build/ at /work. The steps, each skipped while its inputs are unchanged:

  rom       check that the ROM is Super Mario 64 (USA), whatever its byte order
  assets    sm64ex's own Linux build, for the files it extracts and generates from the ROM
  sound     the sound banks again, laid out for a 32-bit machine
  shaders   sm64ex's shaders, compiled for the Zune's GPU (tools/shaders.py)
  stage     sm64ex + its generated files + our patches, platform layer and settings
  compile   Microsoft's VC9 ARM compiler and linker under Wine
  package   the game, the launcher that starts it, and the app's name and icon

Everything under build/ except toolchain/ comes from your ROM. Keep it to yourself.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

import settings
import shaders
import wine

REPO = Path(__file__).resolve().parents[1]
WORK = Path('/work')
TOOLCHAIN = WORK / 'toolchain'
TREE = WORK / 'sm64ex'            # a copy of the submodule that sm64ex's build has run in
ASSETS_LOG = WORK / 'logs/sm64ex-build.log'
SOUND = WORK / 'sound32'
STAGE = WORK / 'stage'            # sm64/, compat/, platform/: what the compiler sees
OBJECTS = WORK / 'obj'
GAME = WORK / 'game'              # nativeapp.exe and its linker map
PACKAGE = WORK / 'package'

US_SHA1 = '9bef1128717f958171a4afac3ed78ee2bb4e86ce'
SOURCE_DIRS = ('include', 'src', 'actors', 'levels', 'bin', 'data', 'lib', 'sound', 'assets', 'text', 'build/us_pc')
# sm64ex's desktop backends, which the platform layer replaces.
DESKTOP_ONLY = re.compile(r'src/pc/(gfx/gfx_(sdl|direct3d|dxgi|opengl_legacy)|audio/audio_sdl|controller/controller_(sdl|emscripten)|discord)')
SOUND_BLOBS = ('sound_data.ctl', 'sound_data.tbl', 'sequences.bin', 'bank_sets')


def say(text):
    print(text, flush=True)


def file_digest(*paths):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(Path(path).read_bytes())
    return digest.hexdigest()


def tree_digest(*roots):
    """A digest of every file under the given directories (names and contents)."""
    digest = hashlib.sha256()
    for root in roots:
        for path in sorted(p for p in Path(root).rglob('*') if p.is_file() and '__pycache__' not in p.parts):
            digest.update(str(path.relative_to(root)).encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def fresh(step, key):
    """True if `step` last completed with this key (its inputs, digested)."""
    stamp = WORK / 'stamps' / step
    return stamp.is_file() and stamp.read_text() == key


def done(step, key):
    stamp = WORK / 'stamps' / step
    stamp.parent.mkdir(exist_ok=True)
    stamp.write_text(key)


# ---------------------------------------------------------------- rom

def read_rom(path):
    """The ROM in big-endian (.z64) byte order, after checking that it is the US version."""
    data = path.read_bytes()
    order = data[:4]
    if order == b'\x37\x80\x40\x12':      # .v64: byte pairs swapped
        swapped = bytearray(data)
        swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
        data = bytes(swapped)
    elif order == b'\x40\x12\x37\x80':    # .n64: little-endian words
        swapped = bytearray(data)
        swapped[0::4], swapped[1::4], swapped[2::4], swapped[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        data = bytes(swapped)
    elif order != b'\x80\x37\x12\x40':
        raise SystemExit('That file is not a Nintendo 64 ROM (.z64, .n64 or .v64).')
    sha1 = hashlib.sha1(data).hexdigest()
    if sha1 != US_SHA1:
        others = {(REPO / f'sm64ex/sm64.{region}.sha1').read_text().split()[0]: name
                  for region, name in (('jp', 'Japanese'), ('eu', 'European'), ('sh', 'Shindou'))}
        what = f'the {others[sha1]} version of Super Mario 64' if sha1 in others else 'not a ROM this project knows'
        raise SystemExit(f'That ROM is {what} (SHA-1 {sha1}).\n'
                         f'Only the US version is supported: SHA-1 {US_SHA1}.')
    return data


# ---------------------------------------------------------------- assets

def build_assets(rom, jobs):
    key = US_SHA1 + file_digest(REPO / 'sm64ex/Makefile', REPO / 'sm64ex/assets.json', REPO / 'sm64ex/extract_assets.py')
    if fresh('assets', key) and ASSETS_LOG.is_file():
        return
    say('Extracting the ROM with sm64ex\'s own build (a few minutes, once)...')
    started = time.time()
    if TREE.exists():
        shutil.rmtree(TREE)
    shutil.copytree(REPO / 'sm64ex', TREE, ignore=shutil.ignore_patterns('.git'))
    (TREE / 'baserom.us.z64').write_bytes(rom)
    ASSETS_LOG.parent.mkdir(exist_ok=True)
    env = dict(os.environ, PATH=f'{TREE}/tools:' + os.environ['PATH'])
    # .SECONDARY keeps the generated sources (the skybox .c files) that make would delete.
    with ASSETS_LOG.open('w') as log:
        result = subprocess.run(['make', 'VERSION=us', f'-j{jobs}', '--eval=.SECONDARY:'], cwd=TREE, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        say(''.join(ASSETS_LOG.read_text(errors='replace').splitlines(True)[-25:]))
        raise SystemExit('sm64ex did not build; the full log is build/logs/sm64ex-build.log')
    done('assets', key)
    say(f'  done in {time.time() - started:.0f} s')


def reference_units():
    """The .c files sm64ex's build compiled, minus tools and desktop backends."""
    units = set()
    for line in ASSETS_LOG.read_text(errors='replace').splitlines():
        if line.startswith('cc -c '):
            source = shlex.split(line)[-1]
            if source.endswith('.c') and not source.startswith('tools/') and not DESKTOP_ONLY.match(source):
                units.add(source)
    return units


def game_units():
    """tools/units.txt, checked against what sm64ex's build really compiled."""
    listed = [line for line in (REPO / 'tools/units.txt').read_text().splitlines() if line and not line.startswith('#')]
    wanted = {u for u in reference_units() if not u.startswith('src/pc/')} | {u for u in listed if u.startswith('src/pc/')}
    if set(listed) != wanted or len(listed) != len(set(listed)):
        missing, extra = sorted(wanted - set(listed)), sorted(set(listed) - wanted)
        raise SystemExit('tools/units.txt does not match sm64ex\'s build.\n'
                         + (f'  not listed: {", ".join(missing)}\n' if missing else '')
                         + (f'  listed but not built by sm64ex: {", ".join(extra)}\n' if extra else ''))
    return listed


# ---------------------------------------------------------------- sound

def build_sound():
    """sm64ex's build ran on a 64-bit machine, so its sound bank, sample table, sequence table
    and bank sets have 64-bit pointer slots. The game patches those slots with the target's
    pointer size, and on the Zune the 64-bit layout sends it chasing garbage on the first note.
    So the two assemble_sound commands of that build are run again for a 32-bit target."""
    key = file_digest(ASSETS_LOG, TREE / 'tools/assemble_sound.py')
    if fresh('sound', key):
        return
    commands = [line for line in ASSETS_LOG.read_text(errors='replace').splitlines()
                if line.startswith('python3 tools/assemble_sound.py ')]
    if len(commands) != 2:
        raise SystemExit(f'Expected 2 assemble_sound commands in sm64ex\'s build log, found {len(commands)}.')
    if SOUND.exists():
        shutil.rmtree(SOUND)
    SOUND.mkdir()
    for command in commands:
        command = command.replace('$(cat build/us_pc/endian-and-bitwidth)', '--endian little --bitwidth 32')
        command, outputs = re.subn(r'build/us_pc/sound/(sound_data\.ctl|sound_data\.tbl|sequences\.bin|bank_sets)\b',
                                   str(SOUND) + r'/\1', command)
        if '--bitwidth 32' not in command or outputs != 2:
            raise SystemExit('Unexpected assemble_sound command: ' + command[:200])
        subprocess.run(command, shell=True, cwd=TREE, check=True)
    for name in SOUND_BLOBS:   # the same text as the Makefile's hexdump rule
        (SOUND / (name + '.inc.c')).write_text(''.join('0x%X,' % b for b in (SOUND / name).read_bytes()) + '\n')
    done('sound', key)


# ---------------------------------------------------------------- shaders

def build_shaders():
    header = WORK / 'shaders/zune_shaders.h'
    key = file_digest(REPO / 'sm64ex/src/pc/gfx/gfx_opengl.c', REPO / 'sm64ex/src/pc/gfx/gfx_pc.c',
                      REPO / 'sm64ex/src/pc/gfx/gfx_cc.h', REPO / 'tools/shaders.py', TOOLCHAIN / 'COMPLETE',
                      *sorted((REPO / 'shaders').glob('zune_overlay.*')))
    if fresh('shaders', key) and header.is_file():
        return header
    say('Compiling shaders for the Zune\'s GPU...')
    count = shaders.build(REPO / 'sm64ex', REPO / 'shaders', TOOLCHAIN / 'nvidia', WORK / 'shaders/work', header)
    done('shaders', key)
    say(f'  {count} shader programs and the touch overlay')
    return header


# ---------------------------------------------------------------- stage

def stage(stamp, chosen, shader_header):
    """Lays out everything the compiler will see and returns the units to compile, as
    (language, path relative to sm64/) in link order."""
    if STAGE.exists():
        shutil.rmtree(STAGE)
    for directory in SOURCE_DIRS:
        for path in sorted((TREE / directory).rglob('*')):
            if path.is_file() and path.suffix in ('.c', '.h'):
                target = STAGE / 'sm64' / path.relative_to(TREE)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
    for name in SOUND_BLOBS:
        shutil.copyfile(SOUND / (name + '.inc.c'), STAGE / 'sm64/build/us_pc/sound' / (name + '.inc.c'))
    for patch in sorted((REPO / 'patches').glob('*.patch')):
        result = subprocess.run(['patch', '-p1', '--quiet', '--forward', '--no-backup-if-mismatch',
                                 '-d', str(STAGE / 'sm64'), '-i', str(patch)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise SystemExit(f'{patch.name} does not apply to sm64ex:\n{result.stdout}')
    shutil.copytree(REPO / 'compat', STAGE / 'compat')
    # CE's own types.h, which compat/types.h hands to windows.h (see that file).
    shutil.copyfile(TOOLCHAIN / 'openzdk/Include/Armv4i/types.h', STAGE / 'compat/ce_types.h')
    shutil.copytree(REPO / 'platform', STAGE / 'platform')
    shutil.copyfile(shader_header, STAGE / 'platform/zune_shaders.h')
    (STAGE / 'platform/zune_build.h').write_text(settings.build_header(stamp, chosen))
    # The game is C89 and builds as C. Then the platform layer: C, C++ (which also wraps
    # sm64ex's C99 renderer core in extern "C"), and the VFP routines in assembly.
    units = [('c', unit) for unit in game_units()]
    for language, pattern in (('c', '*.c'), ('cpp', '*.cpp'), ('asm', '*.asm')):
        units += [(language, '../platform/' + p.name) for p in sorted((REPO / 'platform').glob(pattern))]
    return units


# ---------------------------------------------------------------- compile

def compile_game(units, jobs):
    vc9, sdk = TOOLCHAIN / 'vc9', TOOLCHAIN / 'openzdk'
    source = STAGE / 'sm64'
    environment = {'INCLUDE': wine.path(sdk / 'Include/Armv4i'), 'LIB': wine.path(sdk / 'Lib/Armv4i')}
    includes = [STAGE / 'compat', STAGE / 'platform', source / 'include', source / 'build/us_pc',
                source / 'build/us_pc/include', source / 'src', source, TOOLCHAIN / 'khronos/include']
    # /QRfpe- /QRarch6: ARMv6 with VFP instructions. /GS-: no stack cookies in the audio mixer.
    # sm64_vc9.h (compat/) is included first in every file: it supplies what VC9 lacks.
    flags = ['/nologo', '/c', '/O2', '/fp:fast', '/GR-', '/GS-', '/W3', '/QRfpe-', '/QRarch6', '/FIsm64_vc9.h',
             *['/I' + wine.path(directory) for directory in includes],
             '/DVERSION_US', '/D_LANGUAGE_C', '/DNON_MATCHING', '/DAVOID_UB', '/DF3DEX_GBI_2E', '/DUSE_GLES',
             '/D_WIN32_WCE=0x600', '/DUNDER_CE', '/DWINCE', '/DZUNE_HD', '/D_WINDOWS', '/DARM', '/D_ARM_',
             '/DNDEBUG', '/D_UNICODE', '/DUNICODE', '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_SECURE_NO_DEPRECATE']
    for directory in (OBJECTS, GAME):
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir()
    wine.prepare()

    def compile_unit(unit):
        language, path = unit
        path = path.replace('/', '\\')
        target = OBJECTS / (re.sub(r'[\\/:.]+', '_', path).strip('_') + '.obj')
        if language == 'asm':   # bx needs ARMv5T or later
            code, text = wine.run(vc9 / 'armasm.exe', '-arch', '5T', path, wine.path(target), cwd=source, env=environment)
        else:
            code, text = wine.run(vc9 / 'cl.exe', *flags, '/TP' if language == 'cpp' else '/TC',
                                  '/Fo' + wine.path(target), path, cwd=source, env=environment)
        return path, target, code == 0 and target.is_file(), text

    started = time.time()
    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(compile_unit, units))
    log = WORK / 'logs/compile.log'
    log.write_text(''.join(f'=== {path}\n{text}\n' for path, _, _, text in results))
    failed = [(path, text) for path, _, ok, text in results if not ok]
    for path, text in failed[:10]:
        errors = [line for line in text.splitlines() if 'error' in line.lower()][:15]
        say(f'--- {path}\n' + '\n'.join(errors or text.splitlines()[-15:]))
    if failed:
        raise SystemExit(f'{len(failed)} of {len(units)} files did not compile; the full output is build/logs/compile.log')
    say(f'  compiled {len(units)} files in {time.time() - started:.0f} s')

    response = WORK / 'obj/objects.rsp'
    response.write_text(''.join(f'"{wine.path(target)}"\r\n' for _, target, _, _ in results))
    executable = GAME / 'nativeapp.exe'
    code, text = wine.run(vc9 / 'link.exe', '/NOLOGO', '/OUT:' + wine.path(executable),
                          '/MAP:' + wine.path(GAME / 'nativeapp.map'), '/subsystem:windowsce,6.00',
                          '/ENTRY:wWinMainCRTStartup', '/STACK:1048576,65536', '/OPT:REF', '/OPT:ICF', '/DYNAMICBASE:NO',
                          '/INCREMENTAL:NO', '/NODEFAULTLIB:oldnames.lib', '/NODEFAULTLIB:libcmt.lib',
                          '@' + wine.path(response), 'coredll.lib', 'corelibc.lib', 'zdksystem.lib', 'wininet.lib',
                          env=environment)
    with log.open('a') as handle:
        handle.write('=== link\n' + text)
    wine.finish()
    if code != 0 or not executable.is_file():
        say(text)
        raise SystemExit('The game did not link; the full output is build/logs/compile.log')
    return executable


# ---------------------------------------------------------------- package

def package(executable, stamp, chosen):
    """The folder zune-deploy installs: the XNA launcher (the Zune will only start managed
    programs), the native game it starts, and the app's name and icon."""
    if PACKAGE.exists():
        shutil.rmtree(PACKAGE)
    (PACKAGE / 'data/Content').mkdir(parents=True)
    shutil.copyfile(REPO / 'launcher/application.cfg', PACKAGE / 'application.cfg')
    shutil.copyfile(REPO / 'launcher/thumbnail.png', PACKAGE / 'thumbnail.png')
    shutil.copyfile(REPO / 'launcher/exploiter.exe', PACKAGE / 'data/exploiter.exe')
    shutil.copyfile(executable, PACKAGE / 'data/Content/nativeapp.exe')
    # Crash addresses are looked up in the linker map of the same build (tools/symbolize.py).
    maps = WORK / 'maps'
    maps.mkdir(exist_ok=True)
    shutil.copyfile(GAME / 'nativeapp.map', maps / f'{stamp}.map')
    (WORK / 'package.txt').write_text(f'build {stamp}\nsettings: {settings.summary(chosen)}\n'
                                      f'nativeapp.exe sha256 {file_digest(executable)}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--rom', type=Path, default=Path('/rom'))
    parser.add_argument('--define', action='append', default=[], metavar='NAME=VALUE',
                        help='a build setting from platform/zune_settings.h (repeatable)')
    parser.add_argument('--stamp', default=datetime.now().strftime('%Y%m%d-%H%M%S'),
                        help='the build\'s name, which the game writes to its log')
    parser.add_argument('--jobs', type=int, default=os.cpu_count())
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9A-Za-z_.-]+', args.stamp):
        raise SystemExit(f'--stamp {args.stamp}: letters, digits, dot, dash and underscore only')
    chosen = settings.resolve(args.define)   # first: a wrong setting stops the build at once
    if not (TOOLCHAIN / 'COMPLETE').is_file():
        raise SystemExit('build/toolchain is missing; run ./sm64zune build, which fetches it.')
    started = time.time()
    rom = read_rom(args.rom)
    build_assets(rom, args.jobs)
    build_sound()
    shader_header = build_shaders()
    say(f'Building {args.stamp}: {settings.summary(chosen)}')
    units = stage(args.stamp, chosen, shader_header)
    executable = compile_game(units, args.jobs)
    package(executable, args.stamp, chosen)
    say(f'Built build/package ({executable.stat().st_size // 1024} KiB game) in {time.time() - started:.0f} s')


if __name__ == '__main__':
    sys.exit(main())
