#!/usr/bin/env python3
"""Download the tools this project needs but may not ship, into build/toolchain.

  Microsoft Visual C++ 2008 ARM compiler   12 files (6 MB) picked out of Microsoft's own
                                           Visual Studio 2008 Professional trial ISO: only
                                           the two cabinets that hold them (24 MB of 3.5 GB)
                                           are downloaded, and nothing is installed
  OpenZDK 4.5 headers and libraries        the Windows CE 6 SDK for the Zune HD, from the
                                           archived OpenZDK installer (6 MB)
  NVIDIA cgc and shaderfix                 the Tegra shader compiler, from NVIDIA's Tegra 250
                                           CE6 platform pack (36 MB)
  Khronos EGL and OpenGL ES 2 headers      seven headers (MIT and Apache 2.0 licences)

Every download is checked against a SHA-256 recorded in this file, so a changed file stops
the build. Microsoft's, NVIDIA's and OpenZDK's files come under their owners' licence terms,
which do not allow passing them on: they stay in build/, which git ignores.

Runs inside the build container; ./sm64zune build calls it when build/toolchain is missing
or was made by another version of this file.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

TOOLCHAIN = Path('/work/toolchain')
MARKER = TOOLCHAIN / 'COMPLETE'

VS2008_ISO = ('https://download.microsoft.com/download/8/1/d/81d3f35e-fa03-485b-953b-ff952e402520/'
              'VS2008ProEdition90dayTrialENUX1435622.iso')
# cabinet: offset in the ISO, bytes, SHA-256
CABINETS = {
    'cab17.cab': (131842048, 10547292, 'b08a847207a829fa91620238da63bb8c3921aa5b52331720e4078df792b70229'),
    'cab5.cab': (424294400, 13819163, '96ec5d0d031b5afe0c6cd599792eaa9f778dae39865817e369f76b15fa700d1b'),
}
# cabinet, MSI file key, destination, SHA-256. The x86-hosted ARM compiler is the group of keys
# 112410-112427. Its mspdb80.dll has the key "mspdb71"; the file keyed "mspdb80" in another
# cabinet belongs to the amd64-hosted compiler and does not load.
VC9 = [
    ('cab17.cab', 'FL_cl_exe_112413_', 'vc9/cl.exe', '4304db5927fa3617d4a5dae6e4f0a83884ac7cec3d83a0e225b46b1af08c8fe7'),
    ('cab17.cab', 'FL_c1_dll_112410_', 'vc9/c1.dll', '3baae13aa66c5a7f1b02110717a9fdad4cedd9dd35614802efcb47be40d645de'),
    ('cab17.cab', 'FL_c1xx_dll_112411_', 'vc9/c1xx.dll', '602485c452ae0364247aeacd31723eeef94d6f5520ef0d6ec4f81002976eb0c1'),
    ('cab17.cab', 'FL_c2_dll_112412_', 'vc9/c2.dll', '56f85e91f9bd4713e7246c2816490632e024e70301cb724f08864d600d397456'),
    ('cab17.cab', 'FL_link_exe_112415_', 'vc9/link.exe', '836eb85b874a9b9cf9aa9fd489a94ec77558ab5697b40ba2653c62caf8c6f840'),
    ('cab17.cab', 'FL_armasm_exe_112414_', 'vc9/armasm.exe', 'fba9fd852a7711755c774fd293559c3169e037f59b6b3ec4ea7241197cab7371'),
    ('cab17.cab', 'FL_clui_dll_112427_', 'vc9/1033/clui.dll', '987485bf684bee0c2331759a10e796f5bc7a9160095b458906d4bf2d3488fe4f'),
    ('cab17.cab', 'FL_linkui_dll_102977_', 'vc9/1033/linkui.dll', 'c0f548c34a22bd91647e5acd11366d006e5dd6b4fec0c0a02fb91c14841eb466'),
    ('cab5.cab', 'FL_mspdb71_dll_1_60031_', 'vc9/mspdb80.dll', '4df8c3cacee00a48d528ce58deaff404b8e25b2c31eaf0a3a22871308ca44363'),
    ('cab5.cab', 'FL_msobj80_dll_132026_', 'vc9/msobj80.dll', 'a346ecaeb5a64d92eb1c4969eacedc3b9ee8e944c6395d98d144c58c24b84137'),
    ('cab5.cab', 'FL_mspdbcore_dll_92167_', 'vc9/mspdbcore.dll', '3d6f185d291a3007f443f7ac2d6eb7b43216659eab8f204ab16c9692df7f54f6'),
    ('cab5.cab', 'FL_mspdbsrv_exe_92168_', 'vc9/mspdbsrv.exe', '0477c3eee35889436da8f5ff955fc19f5c53696fffc566caf5313310f5b492d3'),
]

OPENZDK_MSI = ('https://raw.githubusercontent.com/ZuneRedux/openZDK-quick-start-kit/'
               'f15044674798b0ee50e9a95def9f4b9755d90eb3/zune_hd/openzdk-hd-4.5-20100414.msi',
               '2c8f2aa524beef7d6bf96d89c9feb5f577cd8cf93a041ce2a49739ca826fe2c0')
# The import libraries the game links, as a check that the SDK unpacked as expected.
OPENZDK_LIBRARIES = {
    'coredll.lib': 'e4c32d04ab911b9f795ea28f1f1d69faeff8efd06eb5a8cd3c35e913b3fa920d',
    'corelibc.lib': '2609c92f911056b59797d6f1e9c3ea8d15cbe7e8ef19503417dcb03d661ca09d',
    'wininet.lib': '8e88d9d2883969fb29f9494997bfc2fd97811382132860dc0a499ca59c82fcaa',
    'zdksystem.lib': '41728829e571a7017183f3bed351438b0be16d9372363f6371073a737464406c',
}

NVIDIA_MSI = ('https://developer.download.nvidia.com/tegra/files/ce6_tegra_250_5265393.msi',
              'd0c61026136039f2ec7718732d2068526046d8573b582c7b516f5d72807e8eb3')
NVIDIA = {
    'cgc.exe': '98e34515ed59885363c3f710059d0554241f664caea9db440ce375b27e09470d',
    'shaderfix.exe': '9750330910b47a5f87139cabaf43c3ea53447f7e1a83125498e6b53b7cdcb4e2',
}

KHRONOS = 'https://raw.githubusercontent.com/KhronosGroup/'
EGL = 'EGL-Registry/db3425b8246136faccb5e2782b5694960bd6edf1/api/'
GLES = 'OpenGL-Registry/1cdd228e34966dd6b95bd203e9f84faba0f371a1/api/'
# OpenZDK's headers include these but its installer leaves them out.
KHRONOS_HEADERS = {
    EGL + 'EGL/egl.h': 'a7c24c828bf2e1a4dfe6511b4f4812b948a285eb54787074015896976d4da076',
    EGL + 'EGL/eglext.h': 'a5f574a0074001400c6c50232235ee00e9a67b2168bfd1c364ba0f6f52e5a75c',
    EGL + 'EGL/eglplatform.h': '25b5391655effcf363a38fa00c3154f356a3eab3d35dc067847875513cf1e441',
    EGL + 'KHR/khrplatform.h': '7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83',
    GLES + 'GLES2/gl2.h': 'ba5e8e1755642efeb2d0f34b4cfe261cdbf6471e7763a40c7de10dff4c3c4921',
    GLES + 'GLES2/gl2ext.h': '9afc725e9dda7c8b476e7337e75b22fa660ed462c169e203ec28c10e62f91f4c',
    GLES + 'GLES2/gl2platform.h': 'f5da0747540a50be5f44aad264aae45bdf157a192c40f17487dd9a2f99c71b6c',
}


def version():
    """Changes whenever this file does: a toolchain fetched by another version is refetched."""
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def current():
    return MARKER.is_file() and MARKER.read_text().strip() == version()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def download(url, expected, start=None, length=None):
    """The bytes at `url` (or `length` bytes from `start`), which must have SHA-256 `expected`."""
    name = url.rsplit('/', 1)[-1]
    request = urllib.request.Request(url, headers={'User-Agent': 'sm64-zune'})
    if start is not None:
        request.add_header('Range', f'bytes={start}-{start + length - 1}')
    print(f'  downloading {name}' + (f' ({length // 1024} KiB of it)' if start is not None else ''), flush=True)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            if start is not None and response.status != 206:
                raise SystemExit(f'{url}\n  did not honour the request for a part of the file.')
            data = response.read()
    except (urllib.error.URLError, OSError) as error:
        raise SystemExit(f'{url}\n  could not be downloaded: {error}')
    if digest(data) != expected:
        raise SystemExit(f'{url}\n  is not the file this project was written for (SHA-256 {digest(data)},\n'
                         f'  expected {expected}). Not using it.')
    return data


def place(source, destination, expected):
    if digest(source.read_bytes()) != expected:
        raise SystemExit(f'{destination.name} unpacked with an unexpected SHA-256; not using it.')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def unpack_msi(data, work):
    """Unpacks an MSI's files under `work` with their real names and returns that directory."""
    msi = work.with_suffix('.msi')
    msi.write_bytes(data)
    work.mkdir()
    subprocess.run(['msiextract', '-C', str(work), str(msi)], check=True, stdout=subprocess.DEVNULL)
    msi.unlink()
    return work


def only(matches, what):
    matches = list(matches)
    if len(matches) != 1:
        raise SystemExit(f'Expected one {what} in the unpacked installer, found {len(matches)}.')
    return matches[0]


def fetch_vc9(out, work):
    print('Microsoft Visual C++ 2008 ARM compiler (from the Visual Studio 2008 trial ISO)')
    for cabinet, (offset, size, expected) in CABINETS.items():
        (work / cabinet).write_bytes(download(VS2008_ISO, expected, offset, size))
    for cabinet, key, destination, expected in VC9:
        raw = work / 'vc9'
        raw.mkdir(exist_ok=True)
        subprocess.run(['cabextract', '-q', '-F', key + '*', '-d', str(raw), str(work / cabinet)], check=True,
                       stderr=subprocess.DEVNULL)   # cabextract warns about the cabinets' padding
        place(only(raw.glob(key + '*'), key), out / destination, expected)


def fetch_openzdk(out, work):
    print('OpenZDK 4.5: Windows CE 6 headers and import libraries for the Zune HD')
    tree = unpack_msi(download(*OPENZDK_MSI), work / 'openzdk')
    for part in ('Include', 'Lib'):
        source = only((p for p in tree.rglob('Armv4i') if p.parent.name == part), f'{part}/Armv4i directory')
        shutil.copytree(source, out / 'openzdk' / part / 'Armv4i')
    for name, expected in OPENZDK_LIBRARIES.items():
        library = out / 'openzdk/Lib/Armv4i' / name
        if not library.is_file() or digest(library.read_bytes()) != expected:
            raise SystemExit(f'{name} is missing from the OpenZDK installer or differs; not using it.')


def fetch_nvidia(out, work):
    print('NVIDIA Tegra shader compiler (cgc, shaderfix)')
    tree = unpack_msi(download(*NVIDIA_MSI), work / 'nvidia')
    for name, expected in NVIDIA.items():
        matches = [p for p in tree.rglob(name) if digest(p.read_bytes()) == expected]
        if not matches:
            raise SystemExit(f'{name} is missing from the NVIDIA pack or differs; not using it.')
        place(matches[0], out / 'nvidia' / name, expected)


def fetch_khronos(out):
    print('Khronos EGL and OpenGL ES 2 headers')
    for path, expected in KHRONOS_HEADERS.items():
        target = out / 'khronos/include' / path.split('/api/')[1]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(download(KHRONOS + path, expected))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='exit 0 if build/toolchain is complete and current')
    parser.add_argument('--list', action='store_true', help='print what would be downloaded, and stop')
    args = parser.parse_args()
    if args.check:
        sys.exit(0 if current() else 1)
    if args.list:
        print('\n'.join(__doc__.split('\n\n')[1:3]))
        return
    if current():
        return
    if TOOLCHAIN.exists():
        shutil.rmtree(TOOLCHAIN)
    partial = TOOLCHAIN.with_name('toolchain.partial')
    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir(parents=True)
    with tempfile.TemporaryDirectory(dir=partial.parent) as work:
        work = Path(work)
        fetch_vc9(partial, work)
        fetch_openzdk(partial, work)
        fetch_nvidia(partial, work)
        fetch_khronos(partial)
    partial.rename(TOOLCHAIN)
    MARKER.write_text(version() + '\n')
    size = sum(p.stat().st_size for p in TOOLCHAIN.rglob('*') if p.is_file())
    print(f'Tools ready in build/toolchain ({size // (1024 * 1024)} MiB)')


if __name__ == '__main__':
    main()
