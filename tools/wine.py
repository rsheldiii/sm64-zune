"""Running the Windows build tools (VC9, NVIDIA's shader compiler) under Wine."""
import os
from pathlib import Path
import subprocess
import tempfile

PREFIX = Path('/work/.wine')


def environment(extra=None):
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all')
    env.update(extra or {})
    return env


def prepare():
    """Creates the Wine prefix on first use, and keeps one Wine server up for the whole build
    so that each of the hundreds of compiler processes starts in milliseconds."""
    quiet = {'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    if not (PREFIX / 'system.reg').is_file():
        subprocess.run(['wineboot', '--init'], env=environment(), check=True, **quiet)
        subprocess.run(['wineserver', '--wait'], env=environment(), check=True, **quiet)
    subprocess.run(['wineserver', '--persistent'], env=environment(), check=False, **quiet)
    # The first program started under a new server also starts Wine's background services,
    # which keep its output handles for as long as they live. Give them /dev/null now.
    subprocess.run(['wine', 'cmd', '/c', 'exit'], env=environment(), check=False, stdin=subprocess.DEVNULL, **quiet)
    return PREFIX / 'drive_c'


def finish():
    subprocess.run(['wineserver', '--kill'], env=environment(), check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def path(unix):
    """A Unix path as the Windows programs see it: Wine's Z: drive is the root directory."""
    return 'Z:' + str(unix).replace('/', '\\')


def run(program, *arguments, cwd=None, env=None):
    """Runs a Windows program to completion. Returns (exit code, output and errors as text).
    The output goes through a file, not a pipe: a pipe would stay open, and the read would
    hang, if the program left any Wine process behind that inherited it."""
    with tempfile.TemporaryFile() as output:
        result = subprocess.run(['wine', str(program), *arguments], cwd=cwd, env=environment(env),
                                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        output.seek(0)
        return result.returncode, output.read().decode('latin-1').replace('\r\n', '\n')
