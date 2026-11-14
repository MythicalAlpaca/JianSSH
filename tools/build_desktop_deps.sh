#!/usr/bin/env bash
# SDL2 static code + dynamically loaded desktop drivers; never installs host packages.
set -Eeuo pipefail
ROOT=${DESKTOP_DEPS_ROOT:-/home/zhangxx/下载/JianSSH-deps/linux-desktop-glibc}
SOURCE=${SDL_SOURCE:-/home/zhangxx/下载/JianSSH-deps/sdl/SDL2-2.30.12}
ZIG=${ZIG:-$(command -v zig)}
GLIBC_BASELINE=${GLIBC_BASELINE:-2.28}
JOBS=${JOBS:-4}
TOOLS=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$ROOT"/{packages,sysroot,logs,bin,include-host,zig-cache}
exec > >(tee "$ROOT/logs/build.log") 2>&1
trap 'echo "FAILED at line $LINENO (exit $?)"' ERR
printf 'Started: %s\nRoot: %s\nTarget: x86_64-linux-gnu.%s\n' "$(date -Is)" "$ROOT" "$GLIBC_BASELINE"
"$ZIG" version
if [[ -f $SOURCE.tar.gz ]]; then sha256sum "$SOURCE.tar.gz" > "$ROOT/logs/source-sha256.txt"; fi
uname -a > "$ROOT/logs/host-uname.txt"
ldd --version > "$ROOT/logs/host-glibc.txt"
# Download/extract only. No apt install, sudo, or system mutations.
packages=(libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxss-dev libxrender-dev libwayland-dev libwayland-bin libxkbcommon-dev)
(cd "$ROOT/packages"; apt-get download "${packages[@]}")
for deb in "$ROOT"/packages/*.deb; do dpkg-deb -x "$deb" "$ROOT/sysroot"; done
(cd "$ROOT/packages"; sha256sum ./*.deb > SHA256SUMS)
# Dev .deb symlinks refer to runtime SONAMEs, which are intentionally not installed
# into this root. Bridge existing host runtimes explicitly (not copied or linked into SDL).
for link in "$ROOT"/sysroot/usr/lib/x86_64-linux-gnu/*.so; do
    if [[ -L $link && ! -e $link ]]; then
        runtime=$(readlink "$link")
        if [[ -e /usr/lib/x86_64-linux-gnu/$runtime ]]; then
            ln -sfn "/usr/lib/x86_64-linux-gnu/$runtime" "$(dirname "$link")/$runtime"
        fi
    fi
done
# Copy ONLY graphics headers, not /usr/include's libc, to retain Zig's glibc sysroot.
for dir in X11 GL EGL KHR; do
    if [[ -d /usr/include/$dir ]]; then cp -a /usr/include/"$dir" "$ROOT/include-host/"; fi
done
cp -a "$ROOT/sysroot/usr/include/." "$ROOT/include-host/"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/zig-cache"
cat > "$ROOT/bin/zig-cc" <<EOF
#!/bin/sh
exec "$ZIG" cc -target x86_64-linux-gnu.$GLIBC_BASELINE -isystem "$ROOT/include-host" "\$@"
EOF
cat > "$ROOT/bin/zig-cxx" <<EOF
#!/bin/sh
exec "$ZIG" c++ -target x86_64-linux-gnu.$GLIBC_BASELINE -isystem "$ROOT/include-host" "\$@"
EOF
chmod +x "$ROOT/bin/zig-cc" "$ROOT/bin/zig-cxx"
cat > "$ROOT/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$ROOT/bin/zig-cc")
set(CMAKE_CXX_COMPILER "$ROOT/bin/zig-cxx")
set(CMAKE_AR "$ZIG" CACHE FILEPATH "" FORCE)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "$ZIG ranlib <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "$ZIG ranlib <TARGET>")
set(CMAKE_FIND_ROOT_PATH "$ROOT/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
EOF
export PKG_CONFIG_LIBDIR="$ROOT/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$ROOT/sysroot"
export PATH="$ROOT/sysroot/usr/bin:$PATH"
cmake -S "$SOURCE" -B "$ROOT/build" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$ROOT/install" \
    -DCMAKE_LIBRARY_PATH="$ROOT/sysroot/usr/lib/x86_64-linux-gnu;/usr/lib/x86_64-linux-gnu" \
    -DCMAKE_INCLUDE_PATH="$ROOT/include-host" -DX_INCLUDEDIR="$ROOT/include-host" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_OPENGL=ON -DSDL_VULKAN=OFF \
    -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON -DSDL_WAYLAND_LIBDECOR=OFF \
    -DSDL_KMSDRM=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_IBUS=OFF \
    2>&1 | tee "$ROOT/logs/configure.log"
cmake --build "$ROOT/build" --parallel "$JOBS" 2>&1 | tee "$ROOT/logs/compile.log"
cmake --install "$ROOT/build" 2>&1 | tee "$ROOT/logs/install.log"
# Exact generated SDL_config.h is installed with public headers by CMake.
"$ROOT/bin/zig-cc" -O2 "$TOOLS/desktop_gl_smoke.c" -I"$ROOT/install/include/SDL2" \
    "$ROOT/install/lib/libSDL2.a" -lm -ldl -lpthread -lrt -o "$ROOT/bin/desktop-gl-smoke" \
    2>&1 | tee "$ROOT/logs/smoke-link.log"
readelf -d "$ROOT/bin/desktop-gl-smoke" > "$ROOT/logs/smoke-dynamic.txt"
readelf --version-info "$ROOT/bin/desktop-gl-smoke" > "$ROOT/logs/smoke-symbol-versions.txt"
# Preserve what was built; don't let silent backend autodetection satisfy the task.
for feature in SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_WAYLAND SDL_VIDEO_OPENGL SDL_VIDEO_OPENGL_GLX; do
    if ! /usr/bin/awk -v key="$feature" '$1 == "#define" && $2 == key && $3 == 1 {found=1} END {exit !found}' "$ROOT/install/include/SDL2/SDL_config.h"; then
        echo "Required desktop feature missing: $feature"; exit 1
    fi
done
set +e
DISPLAY=${DISPLAY:-:10.0} SDL_VIDEODRIVER=x11 timeout 12s "$ROOT/bin/desktop-gl-smoke" x11 > "$ROOT/logs/smoke-x11.log" 2>&1
x11_rc=$?
printf '%s\n' "X11 smoke exit=$x11_rc" | tee "$ROOT/logs/results.txt"
cat "$ROOT/logs/smoke-x11.log"
if [[ -n ${WAYLAND_DISPLAY:-} ]]; then
    SDL_VIDEODRIVER=wayland timeout 12s "$ROOT/bin/desktop-gl-smoke" wayland > "$ROOT/logs/smoke-wayland.log" 2>&1
    printf 'Wayland smoke exit=%s\n' "$?" | tee -a "$ROOT/logs/results.txt"
else
    echo 'Wayland runtime NOT TESTED: WAYLAND_DISPLAY is unset (no compositor advertised).' | tee "$ROOT/logs/smoke-wayland.log" | tee -a "$ROOT/logs/results.txt"
fi
set -e
(cd "$ROOT"; sha256sum install/lib/libSDL2.a install/include/SDL2/SDL_config.h bin/desktop-gl-smoke > ARTIFACT-SHA256SUMS)
printf 'Finished: %s\n' "$(date -Is)"
exit "$x11_rc"
