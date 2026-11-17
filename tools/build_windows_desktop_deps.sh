#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
root=${JIANSSH_DEPS_ROOT:-$project/deps}
build="$root/sdl/build/windows-x86_64-gnu"
prefix="$root/sdl/install/windows-x86_64-gnu"
mkdir -p "$build"
zig=$(command -v zig)
for compiler in cc c++; do
 printf '#!/bin/sh\nexec "%s" %s -target x86_64-windows-gnu "$@"\n' "$zig" "$compiler" > "$build/$compiler"
 chmod +x "$build/$compiler"
done
cmake -S "$root/sdl/SDL2-2.30.12" -B "$build/cmake" \
 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_SYSTEM_NAME=Windows \
 -DCMAKE_C_COMPILER="$build/cc" -DCMAKE_CXX_COMPILER="$build/c++" \
 -DCMAKE_RC_COMPILER="$zig" '-DCMAKE_RC_COMPILE_OBJECT=<CMAKE_RC_COMPILER> rc /fo <OBJECT> <SOURCE>' \
 -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
cmake --build "$build/cmake" --parallel "${JOBS:-4}"
cmake --install "$build/cmake"
