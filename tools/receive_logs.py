#!/usr/bin/env python3
"""Receive the log a game sends when you leave it (./sm64zune logs).

A game built with ZUNE_LOG=on keeps a log on the Zune, and nothing can read a Zune's files
over USB. So when you leave the game (hold three fingers on the screen) it sends the log over
Wi-Fi to the computer it was built on, and this is the other end (platform/zune_upload.cpp):

  POST /sm64zune/<run>/<file>/<offset>   a piece of log.txt or profile.bin; hex offset
  POST /sm64zune/<run>/done              the game has sent everything

Files are saved as build/logs/zune/<time>-log.txt and <time>-profile.bin, and a crash in the
log is printed with the names of the functions involved.

Anything on your network can reach this port while it is open, so it accepts only those two
file names, a few MiB of each, and writes nowhere else.
"""
import argparse
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import re
import subprocess
import sys

TOOLS = Path(__file__).resolve().parent
PORT = 8767
PIECE = 64 * 1024
FILE_LIMIT = 8 << 20
TOTAL_LIMIT = 256 << 20   # for one run of this program
REQUEST = re.compile(r'/sm64zune/([0-9a-f]{8})/(?:(log\.txt|profile\.bin)/([0-9a-f]{1,8})|(done))')


class Receiver:
    def __init__(self, directory, work):
        self.directory, self.work = directory, work
        self.runs = {}   # run -> {'stem': time, file name: path}
        self.accepted = 0

    def shown(self, path):
        """The path as the person who ran ./sm64zune sees it."""
        return f'build/{path.relative_to(self.work)}'

    def piece(self, run, name, offset, data):
        """Stores one piece; returns an HTTP status."""
        if not data or len(data) > PIECE or offset % PIECE or offset + len(data) > FILE_LIMIT:
            return 400
        if self.accepted + len(data) > TOTAL_LIMIT:
            return 507
        files = self.runs.get(run)
        if files is None:
            if offset:
                return 409   # the middle of a file whose start went somewhere else
            self.directory.mkdir(parents=True, exist_ok=True)
            stem = datetime.now().strftime('%Y%m%d-%H%M%S')
            while list(self.directory.glob(stem + '-*')):
                stem += 'x'
            files = self.runs[run] = {'stem': stem}
        path = files.setdefault(name, self.directory / f'{files["stem"]}-{name}')
        size = path.stat().st_size if path.is_file() else 0
        if offset > size:
            return 409
        # A piece can arrive twice: the game repeats one whose answer it did not get.
        with open(path, 'r+b' if size else 'wb') as file:
            file.seek(offset)
            file.write(data)
            file.truncate()
        self.accepted += len(data)
        return 200

    def done(self, run):
        files = self.runs.pop(run, None)
        if files is None:
            return 409
        for name in ('log.txt', 'profile.bin'):
            if name in files:
                print(f'Received {self.shown(files[name])} ({files[name].stat().st_size // 1024 + 1} KiB)', flush=True)
        if 'log.txt' in files:
            try:
                self.summarize(files['log.txt'], files.get('profile.bin'))
            except Exception as error:   # the files are safe; the game must still hear "received"
                print(f'  (could not summarize it: {error})', flush=True)
        return 200

    def summarize(self, log, profile):
        text = log.read_text(errors='replace')
        builds = re.findall(r'\bbuild ([0-9A-Za-z_.-]+);', text)
        if builds:
            print(f'  from build {builds[-1]}', flush=True)
        if 'native exception' in text:
            print('  The game crashed in this log:', flush=True)
            subprocess.run([sys.executable, str(TOOLS / 'symbolize.py'), str(log)], check=False)
        else:
            print('  No crash in it.', flush=True)
        if profile:
            print(f'  To read the profile: python3 tools/profile.py --log {self.shown(log)} {self.shown(profile)}',
                  flush=True)


def handler(receiver):
    class Handler(BaseHTTPRequestHandler):
        timeout = 30   # one request at a time: a silent client must not hold up the next

        def do_POST(self):
            status = 404
            match = REQUEST.fullmatch(self.path)
            try:
                length = int(self.headers.get('Content-Length', '0'))
            except ValueError:
                length = -1
            if not 0 <= length <= PIECE:
                status, match = 413, None
            if match:
                data = self.rfile.read(length)
                run, name, offset, done = match.groups()
                status = receiver.done(run) if done else receiver.piece(run, name, int(offset, 16), data)
            self.send_response(status)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def do_GET(self):
            body = b'sm64zune log receiver: waiting for a game to send its log.\n'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, default=Path('/work'), help='the build directory')
    parser.add_argument('--port', type=int, default=PORT)
    args = parser.parse_args()
    server = HTTPServer(('', args.port), handler(Receiver(args.work / 'logs/zune', args.work)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
