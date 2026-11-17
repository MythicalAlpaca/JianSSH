#!/usr/bin/env python3
"""Keep only static archives and installed headers from dependency staging."""
import hashlib
import os
import pathlib
import shutil

project = pathlib.Path(__file__).resolve().parents[1]
root = pathlib.Path(os.environ.get('JIANSSH_DEPS_ROOT', project / 'deps'))
for platform, core, sdl in [
    ('linux', root / 'linux-core-glibc/prefix', root / 'linux-desktop-glibc/install'),
    ('windows', root / 'core/out/x86_64-windows-gnu', root / 'sdl/install/windows-x86_64-gnu'),
]:
    libraries = project / 'lib' / platform
    headers = project / 'include/vendor' / platform
    for prefix in (core, sdl):
        if not (prefix / 'include').is_dir():
            raise SystemExit(f'Missing installed dependency headers: {prefix}')
        shutil.copytree(prefix / 'include', headers, dirs_exist_ok=True)
        for archive in (prefix / 'lib').glob('*.a'):
            shutil.copy2(archive, libraries / archive.name)
    if platform == 'linux':
        shutil.copytree(root / 'linux-desktop-glibc/include-host', headers / 'graphics', dirs_exist_ok=True)
    for name in ('vterm', 'sqlite3', 'ssh2', 'mbedtls', 'mbedx509', 'mbedcrypto', 'everest', 'p256m', 'z', 'SDL2'):
        if not (libraries / f'lib{name}.a').is_file():
            raise SystemExit(f'Missing archive: {platform}/lib{name}.a')
    manifest = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in sorted(libraries.glob('*.a')))
    (libraries / 'SHA256SUMS').write_text(manifest)
    print(f'Imported {platform}: {libraries}, {headers}')
