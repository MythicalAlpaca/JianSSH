#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-build}; shift || true
imgui="$PWD/include/imgui"
mkdir -p build/obj zig-out/bin
export ZIG_GLOBAL_CACHE_DIR="$PWD/build/zig-cache"

platform=linux
case "$mode" in
 windows) platform=windows; mode=build;;
esac
if [[ ${JIANSSH_PLATFORM:-} == windows ]]; then platform=windows; fi

if [[ $platform == windows ]]; then
 target=x86_64-windows-gnu
  core=${JIANSSH_CORE_PREFIX:-$PWD}
  extra=${JIANSSH_EXTRA_PREFIX:-$PWD}
  sdl=${JIANSSH_SDL_PREFIX:-$PWD}
  flags=(-target "$target" -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
    -Iinclude -I"$imgui" -Iinclude/vendor/windows -Iinclude/vendor/windows/SDL2 -I"$core/include" -I"$extra/include" -I"$extra/include/imgui"
   -I"$extra/include/libvterm" -I"$extra/include/sqlite" -I"$sdl/include/SDL2"
   -I"$imgui/backends" -Ibuild/generated)
 libs=()
   for p in "$PWD/lib/windows/libimgui.a" "$extra/lib/windows/libvterm.a" "$extra/lib/windows/libsqlite3.a" \
           "$core/lib/windows/libssh2.a" "$core/lib/windows/libmbedtls.a" "$core/lib/windows/libmbedx509.a" \
           "$core/lib/windows/libmbedcrypto.a" "$core/lib/windows/libeverest.a" "$core/lib/windows/libp256m.a" \
           "$core/lib/windows/libz.a"; do
   test -f "$p" || { echo "Missing $p; build the Windows dependency archives first" >&2; exit 1; }
   libs+=("$p")
 done
 link_extra=(-fsanitize=undefined
   -lopengl32 -lgdi32 -luser32 -lwinmm -limm32 -lole32 -loleaut32 -luuid -lcomdlg32
   -ladvapi32 -lcfgmgr32 -lsetupapi -lversion -lbcrypt -lws2_32 -ldxguid -lhid)
   # GUI subsystem: SDL2main supplies WinMain and main maps to SDL_main,
   # so double-clicking the EXE never opens a console window.
   flags+=(-Dmain=SDL_main)
    libs+=("$sdl/lib/windows/libSDL2main.a" "$sdl/lib/windows/libSDL2.a")
   link_extra+=(-Wl,--subsystem,windows)
   binary=zig-out/bin/JianSSH.exe
   objdir=build/obj-windows
else
 target=x86_64-linux-gnu.2.35
  core=${JIANSSH_CORE_PREFIX:-$PWD}
  sdl=${JIANSSH_SDL_PREFIX:-$PWD}
 flags=(-target "$target" -std=c++20 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -pthread
    -Iinclude -I"$imgui" -Iinclude/vendor/linux -Iinclude/vendor/linux/SDL2 -I"$core/include"
    -Iinclude/vendor/linux/graphics -I"$imgui/backends" -Ibuild/generated)
  test -f "$PWD/lib/linux/libimgui.a" || { echo 'Missing lib/linux/libimgui.a' >&2; exit 1; }
  libs=("$PWD/lib/linux/libimgui.a")
  for name in vterm sqlite3 ssh2 mbedtls mbedx509 mbedcrypto everest p256m z; do
    p="$core/lib/linux/lib$name.a"; test -f "$p" || { echo "Missing $p; run tools/build_core_deps.sh" >&2; exit 1; }; libs+=("$p")
 done
  link_extra=("$sdl/lib/linux/libSDL2.a" -ldl -lm)
 binary=zig-out/bin/JianSSH
 objdir=build/obj
fi

if [[ $mode == deps-check ]]; then
  test -f "$sdl/lib/$platform/libSDL2.a" || { echo 'Missing desktop SDL; run tools/build_desktop_deps.sh' >&2; exit 1; }
 echo 'Explicit glibc core and desktop SDL archives present'; exit 0
fi
compile() {
 local src=$1 obj=$2
 if [[ ! -f $obj || $src -nt $obj ]] || python3 -c 'import pathlib,sys; o=pathlib.Path(sys.argv[1]); sys.exit(0 if not o.exists() or any(p.stat().st_mtime>o.stat().st_mtime for r in ("include","build/generated") for p in pathlib.Path(r).rglob("*") if p.is_file()) else 1)' "$obj"; then
   zig c++ "${flags[@]}" -c "$src" -o "$obj"
 fi
}
crypto_dir="build/credentials/$target"
mkdir -p "$crypto_dir"
if [[ ! -f "$crypto_dir/libcredential_crypto.a" || src/zig/crypto.zig -nt "$crypto_dir/libcredential_crypto.a" || ! -f "$crypto_dir/target.txt" || $(<"$crypto_dir/target.txt") != "$target" ]]; then
 zig build-lib src/zig/crypto.zig -target "$target" -lc -O ReleaseSafe -femit-bin="$crypto_dir/libcredential_crypto.a"
 printf '%s\n' "$target" > "$crypto_dir/target.txt"
fi
libs+=("$PWD/$crypto_dir/libcredential_crypto.a")
python3 tools/embed_assets.py
python3 tools/prepare_imgui_backend.py "$imgui/backends/imgui_impl_opengl3.cpp" build/generated/imgui_impl_opengl3.cpp
test -f "$sdl/lib/$platform/libSDL2.a" || { echo 'Real desktop SDL missing, cannot use headless musl archives' >&2; exit 1; }
mkdir -p "$objdir"
if [[ ! -f "$objdir/flags.txt" || $(<"$objdir/flags.txt") != "${flags[*]}" ]]; then
 rm -rf "$objdir"; mkdir -p "$objdir"; printf '%s\n' "${flags[*]}" > "$objdir/flags.txt"
fi
icon_res=
if [[ $platform == windows ]]; then
 icon_res=build/icon.res
 if [[ ! -f $icon_res || assets/icon.ico -nt $icon_res || assets/icon.rc -nt $icon_res ]]; then
   zig rc /fo "$icon_res" assets/icon.rc
 fi
fi
objects=()
for src in src/main.cpp src/ui/workspace.cpp src/ui/key_file_picker.cpp src/app/workspace_controller.cpp src/app/terminal.cpp src/app/host_service.cpp src/app/local_files.cpp src/app/credential_service.cpp src/app/private_key_file.cpp src/metrics/collector.cpp src/store/database.cpp src/net/ssh_session.cpp src/net/serial_session.cpp "$imgui/backends/imgui_impl_sdl2.cpp" build/generated/imgui_impl_opengl3.cpp; do
 obj="$objdir/$(basename "${src%.cpp}").o"
 compile "$src" "$obj"; objects+=("$obj")
done
zig c++ "${flags[@]}" "${objects[@]}" ${icon_res:+"$icon_res"} "${libs[@]}" "${link_extra[@]}" -o "$binary"
if [[ $mode == run ]]; then exec "$binary" "$@"; fi
if [[ $mode == dist ]]; then
 mkdir -p zig-out/dist
 cp "$binary" zig-out/dist/JianSSH-linux-x86_64
 strip --strip-all zig-out/dist/JianSSH-linux-x86_64
 bash tools/verify_single_binary.sh zig-out/dist/JianSSH-linux-x86_64
 sha256sum zig-out/dist/JianSSH-linux-x86_64 > zig-out/dist/SHA256SUMS
fi
