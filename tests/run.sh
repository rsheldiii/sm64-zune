#!/usr/bin/env bash
# The tests that need no Zune. ./sm64zune test runs this in the build container.
#
# They compile the platform layer's header-only logic as C++98 and C89, which is what the
# Zune's compiler accepts, with the address and undefined-behaviour sanitizers.
# LeakSanitizer is off: it needs ptrace, which the container does not allow.
set -euo pipefail
cd /repo
out=/work/tests
rm -rf "$out"
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0
checks=(-Wall -Wextra -Werror -fsanitize=address,undefined)

for name in touch overlay pacing; do
    g++ -std=c++98 "${checks[@]}" "tests/$name.cpp" -o "$out/$name"
    "$out/$name"
done
gcc -std=c89 "${checks[@]}" -Icompat tests/libm.c -lm -o "$out/libm"
"$out/libm"

# The audio mixer patch may not change a single bit of output: the same random calls go
# through sm64ex's mixer.c and through the patched copy, and the two hashes must agree.
mkdir -p "$out/patched/src/pc"
cp sm64ex/src/pc/mixer.c sm64ex/src/pc/mixer.h "$out/patched/src/pc/"
patch --quiet -p1 -d "$out/patched" -i /repo/patches/0009-zune-envmixer-settled-volume.patch
mixer() { gcc -O2 -w -Ism64ex/include -Ism64ex/src/pc "-DMIXER_C=\"$1\"" tests/envmixer.c -o "$2" && "$2"; }
original=$(mixer "$PWD/sm64ex/src/pc/mixer.c" "$out/envmixer-original")
patched=$(mixer "$out/patched/src/pc/mixer.c" "$out/envmixer-patched")
[[ -n $original && $original == "$patched" ]] || { echo "envmixer: the patched mixer differs ($original vs $patched)"; exit 1; }
echo 'envmixer test passed (patched mixer is bit-exact)'

# The launcher must stay the file that is known to start the game (launcher/AssemblyInfo.cs).
echo '759291ad0272d1d35ad987ff0d3b4d86017969bca7662277bcbecead2fa6dfdc  launcher/exploiter.exe' | sha256sum --check --quiet
echo 'launcher is the tested build'

# The build's own tools.
python3 tests/settings_test.py
python3 tests/receive_logs_test.py
