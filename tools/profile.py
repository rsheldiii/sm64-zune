#!/usr/bin/env python3
"""Summarize a sampling profile from a game built with ZUNE_PROFILE=on.

  python3 tools/profile.py [--map build/maps/<build>.map] [--log LOG] PROFILE [--top N]

The game's sampler (platform/zune_profile.cpp) records (pc, lr, phase) little-endian words
about once per millisecond for the game thread, and for the audio worker while it is busy.
Samples outside nativeapp.exe (GL driver, ZDK, kernel) are charged to the game function in
lr where possible, since those are usually leaf calls from our code. `./sm64zune logs`
saves the profile next to the run's log, as build/logs/zune/<time>-profile.bin.

With --log (the same run's log), samples inside coredll are named after the nearest export
the game logged at startup ("export sqrtf 4001xxxx"): coredll's libm is software floating
point, and that is how the sqrtf/sin/cos cost was found. The name is the closest logged
export below the address, so read it as "near", not as proof.
"""
import argparse
import bisect
from collections import Counter
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = re.compile(r'^\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s+f?\s*(\S+)\s*$')
PHASES = {0: 'other', 1: 'game logic', 2: 'render (gfx_run)', 3: 'audio wait', 4: 'EndDraw', 5: 'idle (sleep)'}
WORKER = 0x100


def load_map(path):
    symbols = {}
    text_end = 0
    for line in Path(path).read_text(errors='replace').splitlines():
        match = PUBLIC.match(line)
        if match:
            symbols.setdefault(int(match.group(2), 16), match.group(1))
        match = re.match(r'\s*0001:00000000\s+([0-9a-f]{8})H\s+\.text', line)
        if match:
            text_end = 0x11000 + int(match.group(1), 16)
    addresses = sorted(symbols)
    return addresses, [symbols[a] for a in addresses], text_end


def function(table, address):
    addresses, names, text_end = table
    if not 0x11000 <= address < text_end:
        return None
    return names[bisect.bisect_right(addresses, address) - 1]


EXPORT = re.compile(r'\bexport (\S+) ([0-9a-f]{8})\b')
NEAR = 0x1000   # an export this far below a sample is still worth naming
exports = []    # sorted (address, name) of coredll routines, from --log


def load_exports(path):
    found = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        match = EXPORT.search(line)
        if match and int(match.group(2), 16):
            found[int(match.group(2), 16)] = match.group(1)
    return sorted(found.items())


def map_for_log(log):
    """The linker map of the build that wrote `log`, else the newest. A log can hold several
    runs; the profile is the last one's, so its last "build <name>;" line counts."""
    if log:
        stamps = re.findall(r'\bbuild ([0-9A-Za-z_.-]+);', Path(log).read_text(errors='replace'))
        if stamps and (ROOT / 'build/maps' / (stamps[-1] + '.map')).is_file():
            return ROOT / 'build/maps' / (stamps[-1] + '.map')
    return ROOT / 'build/game/nativeapp.map'


def outside(address):
    index = bisect.bisect_right(exports, (address, '\xff')) - 1
    if index >= 0 and address - exports[index][0] < NEAR:
        return f'[coredll near {exports[index][1]}]'
    return f'[system code {address >> 20:03x}xxxxx]'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--map', type=Path, help="linker map (default: the build named in --log's \"build <name>\" "
                        "line, else the newest build's)")
    parser.add_argument('--top', type=int, default=30)
    parser.add_argument('--log', type=Path, help="the run's log, to name samples in coredll and find the map")
    parser.add_argument('profile', type=Path)
    args = parser.parse_args()
    if args.log:
        exports[:] = load_exports(args.log)
    if not args.map:
        args.map = map_for_log(args.log)
        print(f'Linker map: {args.map.relative_to(ROOT)}')
    table = load_map(args.map)
    data = args.profile.read_bytes()
    samples = [struct.unpack_from('<III', data, i) for i in range(0, len(data) - len(data) % 12, 12)]
    game = [s for s in samples if not s[2] & WORKER]
    worker = [s for s in samples if s[2] & WORKER]
    print(f'{len(samples)} samples: {len(game)} game thread (~{len(game) / 1000:.1f} s), {len(worker)} audio worker')

    phases = Counter(PHASES.get(s[2], str(s[2])) for s in game)
    print('\nGame thread by frame phase:')
    for phase, n in phases.most_common():
        print(f'  {100 * n / len(game):5.1f}%  {phase}')

    def report(title, chosen):
        if not chosen:
            return
        own, charged = Counter(), Counter()
        for pc, lr, _ in chosen:
            name = function(table, pc)
            if name:
                own[name] += 1
                charged[name] += 1
            else:
                caller = function(table, lr)
                own[outside(pc)] += 1
                charged[f'{caller} -> system' if caller else outside(pc)] += 1
        print(f'\n{title}: top functions by samples ({len(chosen)} samples)')
        for name, n in own.most_common(args.top):
            print(f'  {100 * n / len(chosen):5.1f}%  {name}')
        print(f'\n{title}: system time charged to the calling game function')
        for name, n in charged.most_common(args.top):
            if '-> system' in name:
                print(f'  {100 * n / len(chosen):5.1f}%  {name}')

    busy = [s for s in game if s[2] not in (5,)]
    report('Game thread (excluding idle sleep)', busy)
    report('Game thread, game logic phase only', [s for s in game if s[2] == 1])
    report('Game thread, render phase only', [s for s in game if s[2] == 2])
    report('Game thread, EndDraw phase only', [s for s in game if s[2] == 4])
    report('Audio worker', worker)


if __name__ == '__main__':
    main()
