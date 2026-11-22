#!/usr/bin/env bash
# pix68k's RetroArch core; install into RetroPie-Setup/scriptmodules/libretrocores/.
rp_module_id="lr-pix68k"
rp_module_desc="Sharp X68000 - pix68k (68000, 2MB default)"
rp_module_help="BIOS: $biosdir/pix68k/IPLROM.DAT (128KB), CGROM.DAT (768KB). Content: XDF, DIM, D88, M3U, SASI HDF. Disk writes default to read-only; save-copy preserves originals. See README.md."
rp_module_licence="GPL3 https://raw.githubusercontent.com/j3tm0t0/pix68k/main/LICENSE"
rp_module_repo="git https://github.com/j3tm0t0/pix68k.git main"
rp_module_section="exp"
# The shared CPU library currently requires __int128, including when the guest is 68000.
rp_module_flags="!all 64bit"

function depends_lr-pix68k() {
    getDepends cmake ninja-build g++ || return 1
    [[ "$md_mode" == "remove" ]] && return 0
    local version
    version="$(cmake --version | sed -n '1s/^cmake version //p')"
    if ! compareVersions "$version" ge 3.20; then
        md_ret_errors+=("lr-pix68k requires CMake 3.20 or newer (installed: $version). Use a newer OS or CMake.")
        return 1
    fi
    if ! printf 'int main() { __int128 n = 0; return (int)n; }\n' | g++ -x c++ -fsyntax-only -; then
        md_ret_errors+=("lr-pix68k requires a compiler with __int128 support; use a 64-bit userland.")
        return 1
    fi
}

function sources_lr-pix68k() {
    gitPullOrClone
}

function build_lr-pix68k() {
    cmake -S "$md_build" -B "$md_build/build-libretro" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DPIX68K_HOST=OFF -DPIX68K_LIBRETRO=ON || {
        md_ret_errors+=("Unable to configure lr-pix68k.")
        return 1
    }
    cmake --build "$md_build/build-libretro" --target pix68k_libretro --parallel "$__jobs" || {
        md_ret_errors+=("Unable to build lr-pix68k.")
        return 1
    }
    md_ret_require="$md_build/build-libretro/pix68k_libretro.so"
}

function install_lr-pix68k() {
    md_ret_files=(
        'build-libretro/pix68k_libretro.so'
        'build-libretro/pix68k_libretro.info'
        'README.md'
        'retropie'
        'LICENSE'
    )
}

function configure_lr-pix68k() {
    mkRomDir "x68000"
    defaultRAConfig "x68000"
    mkUserDir "$biosdir/pix68k"
    local info_dir="$configdir/all/retroarch/cores"
    if [[ "$md_mode" != "remove" ]]; then
        mkUserDir "$info_dir"
        cp "$md_inst/pix68k_libretro.info" "$info_dir/"
        chown "$__user":"$__group" "$info_dir/pix68k_libretro.info"
    else
        rm -f "$info_dir/pix68k_libretro.info"
    fi
    # addEmulator preserves an existing default, including lr-px68k.
    addEmulator 1 "$md_id" "x68000" "$md_inst/pix68k_libretro.so"
    addSystem "x68000"
}
