#!/usr/bin/env bash
# Rebuild only the glibc core dependencies; no system installation and no SDL.
set -euo pipefail
PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS_ROOT="${JIANSSH_DEPS_ROOT:-$PROJECT_ROOT/deps}"
ROOT="${CORE_BUILD_ROOT:-$DEPS_ROOT/linux-core-glibc}"
PREFIX="${PREFIX:-}"
JOBS="${JOBS:-2}"
PLATFORM=linux
while (($#)); do
  case "$1" in
    --windows) PLATFORM=windows; shift;;
    --prefix) PREFIX="${2:?--prefix requires a path}"; shift 2;;
    --deps-root) DEPS_ROOT="${2:?--deps-root requires a path}"; ROOT="$DEPS_ROOT/linux-core-glibc"; shift 2;;
    --help|-h) echo 'Usage: tools/build_core_deps.sh [--deps-root PATH] [--prefix PATH]'; exit 0;;
    *) echo "Unknown argument: $1" >&2; exit 2;;
  esac
done
PREFIX="${PREFIX:-$ROOT/prefix}"
ROOT="$(realpath -m "$ROOT")"; PREFIX="$(realpath -m "$PREFIX")"
DEPS_ROOT="$(realpath "$DEPS_ROOT")"
ZIG="$(command -v "${ZIG:-zig}")"
TARGET=x86_64-linux-gnu.2.35
SYSTEM=Linux
if [[ $PLATFORM == windows ]]; then
 TARGET=x86_64-windows-gnu
 SYSTEM=Windows
 ROOT="$DEPS_ROOT/windows-core"
 PREFIX="$DEPS_ROOT/core/out/x86_64-windows-gnu"
fi
mkdir -p "$ROOT" "$PREFIX/include" "$PREFIX/lib" "$ROOT/cache/tmp" "$ROOT/logs"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/cache/global" ZIG_LOCAL_CACHE_DIR="$ROOT/cache/local" TMPDIR="$ROOT/cache/tmp" LC_ALL=C
# Unique work directory: never mutate old extracted sources or reuse CMake caches.
WORK="$(mktemp -d "$ROOT/build.XXXXXXXX")"
exec > >(tee "$ROOT/logs/$(basename "$WORK").log") 2>&1
printf 'Target=%s\nPrefix=%s\nWork=%s\n' "$TARGET" "$PREFIX" "$WORK"
"$ZIG" version
python3 - "$DEPS_ROOT" "$WORK" <<'PY'
import sys, pathlib, hashlib, tarfile, zipfile, json
root, work = map(pathlib.Path, sys.argv[1:])
items = [
('zlib','1.3.1','ssh/sources/zlib-1.3.1.tar.gz','9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23','https://zlib.net/fossils/zlib-1.3.1.tar.gz'),
('mbedTLS','3.6.4','ssh/sources/mbedtls-3.6.4.tar.bz2','ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110','https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2'),
('libssh2','1.11.1','ssh/sources/libssh2-1.11.1.tar.gz','d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7','https://libssh2.org/download/libssh2-1.11.1.tar.gz'),
('libvterm','0.3.3','core/sources/libvterm-0.3.3.tar.gz','09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0','https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz'),
('sqlite','3.47.2','core/sources/sqlite-amalgamation-3470200.zip','aa73d8748095808471deaa8e6f34aa700e37f2f787f4425744f53fdd15a89c40','https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip')]
records=[]
for name, version, rel, expected, url in items:
    p=root/rel; actual=hashlib.sha256(p.read_bytes()).hexdigest()
    if actual != expected: raise SystemExit(f'SHA256 mismatch: {p}: {actual}')
    print(f'SHA256 OK {name} {version}: {actual}', flush=True)
    if p.suffix == '.zip':
        with zipfile.ZipFile(p) as z:
            for n in z.namelist():
                if not (work/n).resolve().is_relative_to(work.resolve()): raise ValueError(n)
            z.extractall(work)
    else:
        with tarfile.open(p) as t: t.extractall(work, filter='data')
    records.append(dict(name=name,version=version,archive=str(p),sha256=actual,url=url))
(work/'sources.json').write_text(json.dumps(records,indent=2)+'\n')
PY
# Wrappers keep the target fixed for all feature checks, compilation and linking.
printf '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' "$ZIG" "$TARGET" > "$WORK/cc"
printf '#!/bin/sh\nexec "%s" c++ -target %s "$@"\n' "$ZIG" "$TARGET" > "$WORK/cxx"
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$WORK/ar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$WORK/ranlib"
chmod +x "$WORK/cc" "$WORK/cxx" "$WORK/ar" "$WORK/ranlib"
common=(-DCMAKE_SYSTEM_NAME="$SYSTEM" -DCMAKE_SYSTEM_PROCESSOR=x86_64
 -DCMAKE_C_COMPILER="$WORK/cc" -DCMAKE_CXX_COMPILER="$WORK/cxx"
 -DCMAKE_AR="$WORK/ar" -DCMAKE_RANLIB="$WORK/ranlib" -DCMAKE_BUILD_TYPE=Release
 -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF)
common+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
cmake -S "$WORK/zlib-1.3.1" -B "$WORK/b-zlib" "${common[@]}" -DZLIB_BUILD_EXAMPLES=OFF
cmake --build "$WORK/b-zlib" --target zlibstatic -j "$JOBS"
if [[ -f "$WORK/b-zlib/libz.a" ]]; then cp "$WORK/b-zlib/libz.a" "$PREFIX/lib/"; else cp "$WORK/b-zlib/libzlibstatic.a" "$PREFIX/lib/libz.a"; fi
cp "$WORK/zlib-1.3.1/zlib.h" "$WORK/b-zlib/zconf.h" "$PREFIX/include/"
cmake -S "$WORK/mbedtls-3.6.4" -B "$WORK/b-mbedtls" "${common[@]}" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "$WORK/b-mbedtls" -j "$JOBS"
cmake --install "$WORK/b-mbedtls"
cmake -S "$WORK/libssh2-1.11.1" -B "$WORK/b-libssh2" "${common[@]}" -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DCRYPTO_BACKEND=mbedTLS -DMBEDTLS_INCLUDE_DIR="$PREFIX/include" -DMBEDCRYPTO_LIBRARY="$PREFIX/lib/libmbedcrypto.a" -DENABLE_ZLIB_COMPRESSION=ON -DZLIB_INCLUDE_DIR="$PREFIX/include" -DZLIB_LIBRARY="$PREFIX/lib/libz.a"
cmake --build "$WORK/b-libssh2" -j "$JOBS"
cmake --install "$WORK/b-libssh2"
mkdir -p "$WORK/obj" "$PREFIX/include/imgui" "$PREFIX/include/libvterm" "$PREFIX/include/sqlite"
vt="$WORK/libvterm-0.3.3"; sql="$WORK/sqlite-amalgamation-3470200"
for src in "$vt"/src/*.c; do
 "$WORK/cc" -O2 -fPIC -std=c99 -I"$vt/include" -c "$src" -o "$WORK/obj/vterm-$(basename "$src" .c).o"
done
"$WORK/ar" rcs "$PREFIX/lib/libvterm.a" "$WORK"/obj/vterm-*.o
"$WORK/cc" -O2 -fPIC -DSQLITE_THREADSAFE=1 -c "$sql/sqlite3.c" -o "$WORK/obj/sqlite3.o"
"$WORK/ar" rcs "$PREFIX/lib/libsqlite3.a" "$WORK/obj/sqlite3.o"
cp "$vt"/include/*.h "$PREFIX/include/"
cp "$vt"/include/*.h "$PREFIX/include/libvterm/"
cp "$sql/sqlite3.h" "$sql/sqlite3ext.h" "$PREFIX/include/"
cp "$sql/sqlite3.h" "$sql/sqlite3ext.h" "$PREFIX/include/sqlite/"
# Preserve license texts and exact provenance/configuration, including generated headers.
mkdir -p "$ROOT/licenses"
cp "$WORK/zlib-1.3.1/LICENSE" "$ROOT/licenses/zlib.txt"
cp "$WORK/mbedtls-3.6.4/LICENSE" "$ROOT/licenses/mbedtls.txt"
cp "$WORK/libssh2-1.11.1/COPYING" "$ROOT/licenses/libssh2.txt"
cp "$vt/LICENSE" "$ROOT/licenses/libvterm.txt"
cp "$WORK/sources.json" "$ROOT/SOURCES.json"
{
 printf 'TARGET=%s\nPREFIX=%s\nWORK=%s\nZIG=%s\n' "$TARGET" "$PREFIX" "$WORK" "$ZIG"
 "$ZIG" version; cmake --version
  printf '%s\n' 'Release/-O2, PIC, static; SQLite THREADSAFE=1; ImGui uses bundled archives; mbedTLS default config, programs/tests OFF; libssh2 mbedTLS and zlib ON; zlib examples OFF.'
 printf '%s\n' 'glibc 2.35 is the configured minimum ABI target, NOT evidence of testing on an old distribution. SDL is separate. No upstream test suites run.'
} > "$ROOT/BUILD-CONFIG.txt"
cp "$WORK/b-mbedtls/CMakeCache.txt" "$ROOT/mbedtls-CMakeCache.txt"
cp "$WORK/b-libssh2/CMakeCache.txt" "$ROOT/libssh2-CMakeCache.txt"
cp "$WORK/b-zlib/CMakeCache.txt" "$ROOT/zlib-CMakeCache.txt"
(cd "$PREFIX"; sha256sum lib/*.a) > "$ROOT/ARTIFACT-SHA256SUMS.txt"
printf 'BUILD_OK prefix=%s\n' "$PREFIX"
