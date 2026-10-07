#!/usr/bin/env python3
"""Build settings of the Zune HD port.

platform/zune_settings.h is the list: each setting's description, legal values and default.
A build can override them, weakest first:

  settings.local          NAME=VALUE lines, # comments; in the project root, not tracked by git
  --define NAME=VALUE     on ./sm64zune build, for one build

A value is a number, or one of the words the header lists (ZUNE_FRAME_SKIP=never). Names are
checked against the header, because a misspelt setting would silently change nothing.

  ./sm64zune settings [--define NAME=VALUE]...     every setting and the value a build would use
"""
import argparse
import ipaddress
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'platform/zune_settings.h'
LOCAL = ROOT / 'settings.local'

WORD = re.compile(r'//\s+(-?\d+) = (\w+)')
RANGE = re.compile(r'//\s+(-?\d+)\.\.(-?\d+)\s*$')
ADDRESS = re.compile(r'//\s+IPv4 address\b')


class Setting:
    def __init__(self, name, default, comment):
        self.name, self.comment = name, comment
        self.words = {m.group(2): int(m.group(1)) for m in map(WORD.match, comment) if m}
        ranges = [m for m in map(RANGE.match, comment) if m]
        self.range = (int(ranges[0].group(1)), int(ranges[0].group(2))) if ranges else None
        self.address = any(map(ADDRESS.match, comment))
        if not self.words and not self.range and not self.address:
            raise SystemExit(f'{HEADER.name}: {name} lists neither value words, a range nor "IPv4 address"')
        self.default = default.strip('"') if self.address else int(default)

    def parse(self, text, source):
        """The value for `text`, or exit with what would have been accepted."""
        if self.address:
            try:
                return str(ipaddress.IPv4Address(text)) if text else ''
            except ValueError:
                pass
        elif text in self.words:
            return self.words[text]
        elif re.fullmatch(r'-?\d+', text):
            value = int(text)
            if value in self.words.values() or (self.range and self.range[0] <= value <= self.range[1]):
                return value
        raise SystemExit(f'{source}: {self.name}={text} is not valid; use {self.accepted()}')

    def accepted(self):
        if self.address:
            return 'an IPv4 address such as 192.168.1.20'
        if self.words:
            return ', '.join(f'{word} ({value})' for word, value in sorted(self.words.items(), key=lambda item: item[1]))
        return f'{self.range[0]} to {self.range[1]}'

    def show(self, value):
        if self.address:
            return value or '(none)'
        word = [w for w, v in self.words.items() if v == value]
        return f'{word[0]} ({value})' if word else str(value)

    def literal(self, value):
        """The value as C source."""
        return f'"{value}"' if self.address else str(value)


def catalogue():
    """The settings declared in zune_settings.h, in file order."""
    settings, comment = [], []
    lines = HEADER.read_text().splitlines()
    for index, line in enumerate(lines):
        if line.startswith('//'):
            comment.append(line)
            continue
        declared = re.fullmatch(r'#ifndef (ZUNE_[A-Z0-9_]+)', line)
        if declared and index + 1 < len(lines):
            default = re.fullmatch(rf'#define {declared.group(1)} (-?\d+|"[^"]*")', lines[index + 1])
            if default:
                settings.append(Setting(declared.group(1), default.group(1), comment))
        comment = []
    if not settings:
        raise SystemExit(f'No settings found in {HEADER}')
    return settings


def overrides(defines):
    """(name, value text, source) from settings.local, then from --define; later ones win."""
    found = []
    if LOCAL.is_file():
        for number, line in enumerate(LOCAL.read_text().splitlines(), 1):
            line = line.split('#', 1)[0].strip()
            if line:
                found.append((line, f'{LOCAL.name} line {number}'))
    found += [(define, '--define') for define in defines]
    result = []
    for text, source in found:
        name, equals, value = (part.strip() for part in text.partition('='))
        if not equals or not name:
            raise SystemExit(f'{source}: "{text}" is not NAME=VALUE')
        result.append((name, value, source))
    return result


def resolve(defines=()):
    """[(Setting, value, source)] for every setting; source is 'default' unless overridden."""
    settings = catalogue()
    by_name = {setting.name: setting for setting in settings}
    chosen = {setting.name: (setting.default, 'default') for setting in settings}
    for name, value, source in overrides(defines):
        if name not in by_name:
            raise SystemExit(f'{source}: there is no setting {name}; the settings are {", ".join(by_name)}')
        chosen[name] = (by_name[name].parse(value, source), source.split(' line')[0])
    value = {name: pair[0] for name, pair in chosen.items()}
    if value['ZUNE_PROFILE'] and not value['ZUNE_LOG']:
        raise SystemExit('ZUNE_PROFILE=on needs ZUNE_LOG=on: the profile is sent with the log')
    if value['ZUNE_LOG'] and not value['ZUNE_LOG_HOST']:
        # ./sm64zune passes the address this computer has on its network.
        here = os.environ.get('SM64ZUNE_HOST_ADDRESS', '')
        if not here:
            raise SystemExit('ZUNE_LOG=on: this computer\'s network address could not be worked out; '
                             'add --define ZUNE_LOG_HOST=<its IPv4 address>')
        chosen['ZUNE_LOG_HOST'] = (by_name['ZUNE_LOG_HOST'].parse(here, 'this computer'), 'this computer')
    return [(setting, *chosen[setting.name]) for setting in settings]


def describe(defines=()):
    """Every setting with its description and the value the next build would use."""
    blocks = []
    for setting, value, source in resolve(defines):
        note = '' if source == 'default' else f'   <- from {source}; default {setting.show(setting.default)}'
        blocks.append('\n'.join([f'{setting.name} = {setting.show(value)}{note}']
                                + ['   ' + line[2:].rstrip() for line in setting.comment]))
    return '\n\n'.join(blocks)


def summary(chosen):
    return ', '.join(f'{setting.name}={setting.show(value)}' + ('' if source == 'default' else f' [{source}]')
                     for setting, value, source in chosen)


def build_header(stamp, chosen):
    """platform/zune_build.h for one build: its name and every setting, spelt out."""
    lines = ['#ifndef ZUNE_BUILD_H', '#define ZUNE_BUILD_H',
             '// Written by tools/build.py for this build: its name, and the value of every setting.',
             f'#define ZUNE_BUILD "{stamp}"']
    lines += [f'#define {setting.name} {setting.literal(value)}' for setting, value, _ in chosen]
    text = ' '.join(f'{setting.name}={value}' for setting, value, _ in chosen)   # for the log's first lines
    return '\n'.join(lines + [f'#define ZUNE_SETTINGS "{text}"', '#endif', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--define', action='append', default=[], metavar='NAME=VALUE')
    print(describe(parser.parse_args().define))


if __name__ == '__main__':
    main()
