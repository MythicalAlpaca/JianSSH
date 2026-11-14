#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
binary=${1:-zig-out/bin/JianSSH}
test -f "$binary"
python3 - "$binary" <<'PY'
import subprocess,re,sys,os
p=sys.argv[1]
env=dict(os.environ,LC_ALL='C')
dynamic=subprocess.check_output(['readelf','-d',p],text=True,env=env)
needed=re.findall(r'Shared library: \[(.*?)\]',dynamic)
assert needed, 'Expected dynamic glibc dependencies; inspect parser or wrong artifact'
allowed={'libc.so.6','libm.so.6','libpthread.so.0','libdl.so.2','librt.so.1','ld-linux-x86-64.so.2'}
assert not set(needed)-allowed, f'Unexpected shared dependencies: {needed}'
assert '(RPATH)' not in dynamic and '(RUNPATH)' not in dynamic
print('ELF explicit static application libraries; system NEEDED:', ', '.join(needed))
print('NOTE: runtime SDL dlopen graph is not covered by ELF NEEDED. X11/Wayland/GL must be tested separately.')
PY
"$binary" --version
