#!/usr/bin/env bash
# Build a single-core RetroArch VPK. Does not install or publish it.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$repo_root/build/vita}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
revision=69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576
ra="$output/retroarch"
launcher="$output/launcher"
cmake -S "$repo_root/libretro/vita" -B "$output/core" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$output/core" --parallel "${PIX68K_BUILD_JOBS:-4}"
if [[ ! -d "$ra/.git" ]]; then
  git clone --depth 1 --branch v1.22.2 https://github.com/libretro/RetroArch.git "$ra"
fi
if [[ "$(git -C "$ra" rev-parse HEAD)" != "$revision" ]]; then
  echo "RetroArch revision differs; use a fresh output directory." >&2
  exit 1
fi
mkdir -p "$launcher"
git -C "$ra" archive "$revision" | tar -xf - -C "$launcher"
python3 "$repo_root/libretro/vita/patch_retroarch.py" "$ra" "$launcher"

cp "$output/core/libretro_vita.a" "$ra/libretro_vita.a"
# Upstream's ELF target does not list libretro_vita.a as a prerequisite.
rm -f "$ra/retroarch_vita.elf" "$ra/retroarch_vita.self" "$ra/retroarch_vita.vpk"
make -C "$ra" -f Makefile.vita -j "${PIX68K_BUILD_JOBS:-4}" \
  'LDFLAGS=-Wl,-q,--pic-veneer -flto -mcpu=cortex-a9' \
  VITA_TITLE_ID=PIX68K001 'VITA_TITLE_NAME=pix68k RetroArch'
make -C "$launcher" -f Makefile.vita.salamander -j "${PIX68K_BUILD_JOBS:-4}"
vita-mksfoex -s TITLE_ID=PIX68K001 -s APP_VER=00.01 'pix68k RetroArch' \
  -d ATTRIBUTE2=12 "$output/param.sfo"
python3 - "$repo_root" "$output" <<'PY'
from pathlib import Path
import sys, zipfile
repo, out = map(Path, sys.argv[1:])
with zipfile.ZipFile(out / 'pix68k_retroarch.vpk', 'w', compression=zipfile.ZIP_DEFLATED) as z:
    for source, target in [
        (out / 'launcher/eboot.bin', 'eboot.bin'),
        (out / 'retroarch/retroarch_vita.self', 'pix68k_libretro.self'),
        (out / 'param.sfo', 'sce_sys/param.sfo'),
        (repo / 'libretro/pix68k_libretro.info', 'info/pix68k_libretro.info'),
        (repo / 'LICENSE', 'LICENSE'),
        (out / 'retroarch/COPYING', 'COPYING.RetroArch'),
    ]:
        z.write(source, target)
print(out / 'pix68k_retroarch.vpk')
PY
