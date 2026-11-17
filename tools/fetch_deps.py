#!/usr/bin/env python3
"""Download pinned dependency sources into the persistent project deps directory."""
import hashlib
import pathlib
import os
import subprocess
import tarfile

root = pathlib.Path(os.environ.get('JIANSSH_DEPS_ROOT', pathlib.Path(__file__).resolve().parents[1] / 'deps'))
items = [
 ('ssh/sources/zlib-1.3.1.tar.gz', 'https://zlib.net/fossils/zlib-1.3.1.tar.gz', '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23'),
 ('ssh/sources/mbedtls-3.6.4.tar.bz2', 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2', 'ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110'),
 ('ssh/sources/libssh2-1.11.1.tar.gz', 'https://libssh2.org/download/libssh2-1.11.1.tar.gz', 'd9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7'),
 ('core/sources/libvterm-0.3.3.tar.gz', 'https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz', '09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0'),
 ('core/sources/sqlite-amalgamation-3470200.zip', 'https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip', 'aa73d8748095808471deaa8e6f34aa700e37f2f787f4425744f53fdd15a89c40'),
 ('sdl/SDL2-2.30.12.tar.gz', 'https://www.libsdl.org/release/SDL2-2.30.12.tar.gz', 'ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea'),
]

def download(item):
    rel, url, expected = item
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        subprocess.run(['curl', '-sSL', '--fail', '--retry', '5', '--retry-all-errors', url, '-o', str(path)], check=True)
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f'SHA256 mismatch: {path}: {actual}')
    print(f'SHA256 OK {rel}', flush=True)

if __name__ == '__main__':
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(download, items))
    for rel, dest in [('sdl/SDL2-2.30.12.tar.gz', 'sdl')]:
        target = root / dest
        target.mkdir(parents=True, exist_ok=True)
        with tarfile.open(root / rel) as archive:
            archive.extractall(target, filter='data')
