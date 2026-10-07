#!/usr/bin/env python3
"""Map code addresses from a crash log (or a crash message on the Zune) to functions.

  python3 tools/symbolize.py [--map build/maps/<build>.map] LOG|ADDRESS...

Given a log file, every 8-digit hex word on the crash lines (pc, lr, stack code addresses) is
looked up; given bare addresses, just those. The image is linked at a fixed base
(/DYNAMICBASE:NO), so map addresses are the runtime addresses. With --disassemble, the
"code <addr>: words" dumps of system DLL code in the log are disassembled (ARMv6, llvm-mc).
"""
import argparse
import bisect
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = re.compile(r'^\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s+f?\s*(\S+)\s*$')
CRASH_LINE = re.compile(r'native exception|stack code addresses')
WORD = re.compile(r'\b([0-9a-f]{8})\b')


def load_map(path):
    symbols = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        match = PUBLIC.match(line)
        if match:
            name, address, obj = match.groups()
            symbols.setdefault(int(address, 16), (name, obj))   # identical-COMDAT folds share an address
    addresses = sorted(symbols)
    return addresses, [symbols[a] for a in addresses]


def lookup(table, address):
    addresses, symbols = table
    index = bisect.bisect_right(addresses, address) - 1
    if index < 0 or address - addresses[index] > 0x10000:
        return None
    name, obj = symbols[index]
    return f'{name}+0x{address - addresses[index]:x} ({obj})'


CODE_LINE = re.compile(r'\bcode ([0-9a-f]{8}):((?: [0-9a-f]{8})+)')
PC_LR = re.compile(r'pc ([0-9a-f]{8}) lr ([0-9a-f]{8})')


def disassemble(log_lines):
    words, marks = {}, {}
    for line in log_lines:
        for match in PC_LR.finditer(line):
            marks[int(match.group(1), 16)] = '<== pc'
            marks.setdefault(int(match.group(2), 16) - 4, '<== call before lr')
        match = CODE_LINE.search(line)
        if match:
            address = int(match.group(1), 16)
            for i, word in enumerate(match.group(2).split()):
                words[address + 4 * i] = int(word, 16)
    previous = None
    for address in sorted(words):
        if previous is not None and address != previous + 4:
            print('  ...')
        previous = address
        word = words[address]
        text = subprocess.run(['llvm-mc', '--disassemble', '-triple=armv6-none-eabi'],
                              input=' '.join(f'0x{b:02x}' for b in word.to_bytes(4, 'little')),
                              capture_output=True, text=True).stdout
        instruction = ' '.join(line.strip() for line in text.splitlines() if line.strip() and not line.strip().startswith('.text'))
        print(f'  {address:08x}  {word:08x}  {instruction or "(data)":40} {marks.get(address, "")}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--map', type=Path, help='linker map (default: build/maps/<build>.map for the build '
                        'named in the log\'s "build <name>" line, else the newest build\'s)')
    parser.add_argument('--disassemble', action='store_true', help='disassemble the logged system DLL code')
    parser.add_argument('inputs', nargs='+')
    args = parser.parse_args()
    if not args.map:
        args.map = ROOT / 'build/game/nativeapp.map'
        for item in args.inputs:
            if Path(item).is_file():
                # A log can hold several runs: take the build that ran last before the first crash.
                text = Path(item).read_text(errors='replace')
                crash = CRASH_LINE.search(text)
                stamps = re.findall(r'\bbuild ([0-9A-Za-z_.-]+);', text[:crash.start()] if crash else text)
                if stamps and (ROOT / 'build/maps' / (stamps[-1] + '.map')).is_file():
                    args.map = ROOT / 'build/maps' / (stamps[-1] + '.map')
                elif stamps:
                    print(f'The log is from build {stamps[-1]}, whose linker map is not in build/maps/. '
                          'The names below come from the newest build here and are wrong unless it is the same code.')
    if not args.map.is_file():
        raise SystemExit(f'No linker map at {args.map}: the map of the build that crashed is needed (--map), '
                         'and build/maps/ keeps one for every build made here.')
    print(f'Linker map: {args.map}')
    table = load_map(args.map)
    words = []
    for item in args.inputs:
        if Path(item).is_file():
            lines = Path(item).read_text(errors='replace').splitlines()
            if args.disassemble:
                disassemble(lines)
            for line in lines:
                if CRASH_LINE.search(line):
                    print(line)
                    words.extend(WORD.findall(line))
        else:
            words.append(item.lower().removeprefix('0x').rjust(8, '0'))
    seen = set()
    for word in words:
        if word in seen:
            continue
        seen.add(word)
        where = lookup(table, int(word, 16))
        if where:
            print(f'  {word}  {where}')


if __name__ == '__main__':
    main()
