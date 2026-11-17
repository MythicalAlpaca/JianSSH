# JianSSH

[简体中文](README.md) | **English**

A lightweight SSH desktop client built with C++20, Zig, Dear ImGui, SDL2, libssh2, mbedTLS, libvterm, and SQLite.

> **Project status: 1.0.0 stable.** Both Linux x86_64 and Windows x86_64 have been validated on real hardware. On Windows the credential vault, SSH Agent, local chmod, and system-theme detection remain unavailable or limited; see Platform support and Security model below. Features and data formats are stable for this release and may continue to evolve.

## Features

### Hosts and sessions

- Add, edit, delete, group, and search hosts. Host profiles are stored in SQLite (WAL mode) and contain no password field.
- Quick connect and multiple session tabs support SSH or serial connections on Linux and Windows. Refresh the port list or enter a device path manually; the default is 115200 8N1 with no flow control. Save a serial device to retain its settings; quick connections are not automatically saved.
- Serial sessions reuse the terminal but do not expose SSH authentication, host-key verification, SFTP, or remote monitoring. Queued unsent input is discarded on disconnect with a notice.
- Opening a port may change DTR/RTS and reset attached hardware. On Linux, configure device permissions yourself (commonly via the `dialout` group or device ACL); the app does not change permissions or send login commands.
- Password, public key (an optional matching `.pub` file may sit next to the private key), and SSH Agent (Linux) authentication. Private keys are selected with a file chooser and checked for readability and plausible format; the Linux chooser requires `zenity`. Legacy keyboard-interactive hosts must be edited and saved with another authentication method before connecting.
- The private-key check is a preliminary format check, not verification of its passphrase, cryptographic validity, or algorithm compatibility; SSH authentication performs the final check.
- On first connection, the SHA256 host-key fingerprint is shown and must be confirmed. Authentication starts only after the trust record is committed. A changed known key is rejected immediately.
- Host keys are stored as application-internal "raw public key + host/port" records; OpenSSH `known_hosts` files are neither read nor rewritten.

### Terminal

- Background, non-blocking SSH I/O with a single owner thread; interactive PTY, window resize, and bounded output backpressure.
- libvterm-based terminal rendering with a 2000-line scrollback limit.
- Input, paste, and PTY resize never block the UI thread; paste is previewed and size-limited before confirmation.
- No automatic reconnect and no replay of previous input. Reconnecting is always an explicit user action.

### File transfer

- Dual-pane SFTP browser with clickable parent directory and drag-and-drop transfers between the local and remote panes.
- Regular-file upload, download, and cancellation, plus mkdir, rename, delete, and chmod.
- Existing files are not overwritten by default. Recursive transfer/delete, resume, and concurrent transfers are not implemented.
- Transfer temporary files use `0600` permissions; failures never "roll back" by deleting the destination.

### Remote monitoring

- Monitoring starts automatically after a successful connection and samples a remote Linux host through SSH exec (`/proc`, `df`, and related sources).
- Latest CPU, memory, disk, and network values appear in the bottom resource bar. No simulated data is shown.
- Failed samples retry with backoff. After disconnection or when monitoring stops, only historical values remain and are marked stale.

### Connection secrets

- Three policies: never remember, current process only, or encrypted connection-secret storage. No master-password setup or unlock is required.
- Secrets are saved only after successful authentication. Passwords and private-key passphrases are supported; OTP responses are never stored. Secret input supports visible/masked display.
- Quick connect has a “Save to workspace devices” checkbox; leaving it unchecked creates no device. Remembering secrets requires a saved device.
- A random local key and XChaCha20-Poly1305 protect records. The directory is `0700`; key, data and lock files are `0600`. An exclusive lock prevents concurrent writes.
- Old master-password vaults are not migrated or deleted. Re-enter secrets and select the new policy.
- Windows has no credential-backend implementation, so none of the secret-memory policies work there. Re-enter passwords on every connection; passwords and key passphrases are never written to disk.

### Interface

- Embedded CJK and proportional UI fonts with a macOS-style neutral material, system-blue accent, and rounded controls; system, dark, and light modes.
- Time-bounded transitions for entry, hover, selection, modal and page changes. Page changes use a quick-out, slow-in two-phase ease. Power saving is enabled by default and rendering is on demand while idle.
- Supports `--reduced-motion` and an in-app "reduce motion" setting.

## Platform support

| Target | Build | Runtime validation | Notes |
| --- | --- | --- | --- |
| Linux x86_64 | ✅ | ✅ Validated on hardware | Primary target; requires glibc ≥ 2.35 and X11/Wayland + OpenGL3 |
| Windows x86_64 | ✅ | ✅ Validated on hardware | GUI subsystem, single EXE; vault, SSH Agent, chmod, and system-theme detection are unavailable or limited |

The Windows target uses prebuilt static archives from the dependency root and never downloads dependencies implicitly; a missing archive fails the build with a clear error. Apart from the UCRT and system DLLs, it does not require `SDL2.dll`.

## Getting started

### Build requirements

- Zig `0.17.0-dev.1683+5ceec001b` (this exact pinned version is required)
- Python 3, CMake, and make
- On Linux: `apt-get` and `dpkg-deb` (used only to download and unpack graphics development packages; nothing is installed system-wide)
- Network access for dependency source archives and graphics development packages

### Prepare dependencies

Static archives are bundled in `lib/linux/` and `lib/windows/`, with matching headers in `include/vendor/<platform>/`. ImGui headers and required SDL/OpenGL backends live in `include/imgui/`. Normal application builds do not need dependency sources. To rebuild dependencies using the temporary `deps/` staging directory:

```bash
python3 tools/fetch_deps.py
./tools/build_core_deps.sh       # zlib / mbedTLS / libssh2 / libvterm / SQLite
./tools/build_desktop_deps.sh    # SDL2 static code + desktop graphics headers
bash tools/build_core_deps.sh --windows
bash tools/build_windows_desktop_deps.sh
python3 tools/import_deps.py
```

Both scripts verify source SHA256 checksums and build only inside the dedicated dependency root; they do not modify the system environment.

### Build and run

```bash
./build.sh deps-check            # Verify explicit static archives are present
./build.sh                       # Build all supported targets sequentially: Linux and Windows
./build.sh linux                 # Linux desktop build → zig-out/bin/JianSSH
./zig-out/bin/JianSSH

./build.sh run                   # Build and run
./build.sh dist-linux-x86_64     # Stripped release build → zig-out/dist/JianSSH-linux-x86_64
./build.sh windows               # Windows x86_64 GUI release → zig-out/bin/JianSSH.exe
```

The default build requires both Linux and Windows static dependencies and stops on the first failure. `./build.sh all` is equivalent to the default; additional Zig build options may follow the target argument. Only the two x86_64 desktop targets above are currently supported; macOS is not included.

The final link uses explicit static archives only. At runtime the application still depends on the system glibc, X11/Wayland, and OpenGL drivers. Fonts and icons are embedded at build time, so no sidecar resources are needed at runtime.

### Command-line options

```text
JianSSH [--smoke-ms milliseconds] [--screenshot file.bmp] [--metrics file]
        [--data-dir directory] [--reduced-motion]
        [--theme system|dark|light] [--window WxH]
```

| Option | Description |
| --- | --- |
| `--version` | Print version and target platform, then exit |
| `--help` | Print usage, then exit |
| `--window WxH` | Initial window size, from `300x200` to `8192x8192` |
| `--theme system\|dark\|light` | Override the theme setting |
| `--reduced-motion` | Reduce motion |
| `--data-dir directory` | Use `jianssh.db` inside the given directory (the vault lives there too) |
| `--smoke-ms N` | Exit automatically after N milliseconds, range `0..3600000` |
| `--screenshot file.bmp` | Save a BMP screenshot before the smoke run exits; requires `--smoke-ms` |
| `--metrics file` | On exit, write a JSON summary with frame count, wait count, elapsed time, and theme |

### Environment variables

| Variable | Description |
| --- | --- |
| `JIANSSH_TIMING=1` | Print startup phase timings to stderr |
| `JIANSSH_DEPS_ROOT` | Dependency root directory |
| `JIANSSH_CORE_PREFIX` | Override the core static-library prefix |
| `JIANSSH_SDL_PREFIX` | Override the SDL2 installation prefix |
| `JIANSSH_EXTRA_PREFIX` | Override the extra dependency prefix for the Windows target |
| `JIANSSH_PLATFORM=windows` | Force the Windows target when invoking `tools/build_project.sh` |

## Data locations

| Content | Linux | Windows |
| --- | --- | --- |
| Host database | `${XDG_CONFIG_HOME:-$HOME/.config}/JianSSH/jianssh.db` | `%APPDATA%\JianSSH\jianssh.db` |
| Encrypted vault | `credentials.enc` (key: `credentials.enc.key`) in the database directory | Unavailable (DPAPI not implemented) |

The immediate parent directory of the database must be dedicated to this application. The program tightens it to `0700`; database, WAL, SHM, and vault files are `0600`. Do not point `--data-dir` at a shared directory or place symlinked files there.

Saving serial devices upgrades the database schema to version 3 while preserving existing SSH profiles and host trust. Older JianSSH versions cannot open the upgraded database; back up the entire application data directory before upgrading if rollback is needed.

## Project layout

```text
.
├── build.zig                 # Zig build entry point (calls tools/build_project.sh)
├── build.sh                  # Convenience wrapper: ./build.sh [all|linux|windows|run|dist-linux-x86_64|deps-check]
├── include/jianssh/          # Public headers
│   ├── app/                  # Session, terminal, credential, and local-file worker APIs
│   ├── net/                  # SSH session and task API
│   ├── store/                # Host and host-key storage
│   ├── ui/                   # UI model, theme, layout, and motion
│   ├── metrics/              # Remote sample parsing
│   └── util/                 # Zig/C++ crypto bridge
├── src/
│   ├── main.cpp              # SDL/ImGui startup, render loop, and CLI parsing
│   ├── app/                  # Worker and controller implementations
│   ├── net/                  # Non-blocking libssh2 session implementation
│   ├── store/                # SQLite storage implementation
│   ├── ui/                   # ImGui interface implementation
│   ├── metrics/              # Sampling script and parser
│   └── zig/crypto.zig        # Argon2id, XChaCha20-Poly1305, secure wipe
├── assets/                   # Icons, fonts, and font licenses
├── tools/                    # Dependency builds, asset embedding, icon generation, artifact checks
├── third_party/licenses/     # Full third-party license texts
├── NOTICE                    # Third-party attribution
├── LICENSE                   # MIT
├── README.md                 # 简体中文
└── README.en.md              # English
```

## Architecture

- The **UI main thread** handles SDL events, ImGui rendering, and model snapshots only. It never performs file, SQLite, or network I/O directly.
- **HostService** owns a single thread for SQLite access. The UI submits commands through a bounded queue and polls immutable snapshots.
- **CredentialService** initializes the local key and runs encryption/decryption and secret I/O on a dedicated worker; process secrets are wiped at shutdown.
- **SshSession** drives libssh2 on a dedicated I/O owner thread. Input, output, and event queues are bounded and apply backpressure when full.
- **LocalFiles** and **Collector** handle local file operations and remote sample parsing off the UI thread.
- **On-demand rendering** lowers the frame rate or waits for events when there is no animation or new input. The font atlas is rasterized on a background thread while the window and GL context are created; the CPU bitmap is released after the first GPU upload.

## Security model and boundaries

- Encrypted credentials use a Linux local key. Anyone holding both ciphertext and key can decrypt them; this does not replace account or disk security. Rollback detection and `mlock` are not provided, so an attacker controlling the current user can access secrets. Windows has no DPAPI (or equivalent) backend, so credentials are never persisted there.
- Host keys are stored only in the application database. Importing OpenSSH hashed `known_hosts` and a key-replacement UI are not supported.
- Select visible terminal text by dragging and use the right-click menu to copy or paste. Ctrl+Shift+C copies, and Ctrl+Shift+V or Shift+Insert pastes (confirmation required). Ctrl+C still sends an interrupt. Search, full scrollback browsing, and comprehensive compatibility regression are still missing; vim, tmux, and IME behavior require continued validation.
- Advanced SFTP capabilities (concurrency, pause, resume, recursive directories, and so on) are not implemented and are planned for a future release.
- Remote monitoring supports Linux targets only. macOS/Windows targets, multi-hop bastions, proxies, and port forwarding are not implemented.
- Windows artifacts have been validated on real hardware. The vault, SSH Agent, and local chmod are unavailable on that target, and system-theme detection is unavailable (System mode falls back to dark).
- There is no auto-update, telemetry, or cloud sync. All host profiles and credentials stay on the local machine.

## Contributing

- Run `./build.sh deps-check` and `./build.sh` before submitting changes to confirm that explicit dependency archives and link paths are intact.
- Dependency versions are pinned. When upgrading, also update `tools/build_core_deps.sh`, `tools/build_desktop_deps.sh`, `NOTICE`, and `third_party/licenses/`.
- Do not commit `build/`, `.zig-cache/`, `zig-out/`, databases, or vault files (they are covered by `.gitignore`).

## Third-party components and licenses

| Component | Version | License | License text |
| --- | --- | --- | --- |
| libssh2 | 1.11.1 | BSD-3-Clause | [third_party/licenses/libssh2.txt](third_party/licenses/libssh2.txt) |
| mbedTLS | 3.6.4 | Apache-2.0 | [third_party/licenses/mbedTLS.txt](third_party/licenses/mbedTLS.txt) |
| zlib | 1.3.1 | zlib | [third_party/licenses/zlib.txt](third_party/licenses/zlib.txt) |
| SDL2 | 2.30.12 | zlib | [third_party/licenses/SDL2.txt](third_party/licenses/SDL2.txt) |
| Dear ImGui | 1.91.9b-docking | MIT | [third_party/licenses/ImGui.txt](third_party/licenses/ImGui.txt) |
| libvterm | 0.3.3 | MIT | [third_party/licenses/libvterm.txt](third_party/licenses/libvterm.txt) |
| SQLite | 3.47.2 | Public Domain | [sqlite.org/copyright.html](https://sqlite.org/copyright.html) |
| DejaVu Sans / DejaVu Sans Mono | — | Bitstream Vera / Public Domain | [assets/fonts/DejaVu-COPYRIGHT](assets/fonts/DejaVu-COPYRIGHT) |
| Droid Sans Fallback | — | Apache-2.0 | [assets/fonts/Droid-COPYRIGHT](assets/fonts/Droid-COPYRIGHT) |

Runtime system X11/Wayland/OpenGL libraries are not distributed with this project. See [NOTICE](NOTICE) for the complete attribution and redistribution notes.

## License and citation

JianSSH is released under the [MIT License](LICENSE). Copyright (c) 2026 JianSSH contributors.

If you cite this project in a paper, report, or derivative work:

```bibtex
@misc{jianssh,
  title        = {JianSSH: A Lightweight SSH Desktop Client},
  author       = {{JianSSH contributors}},
  year         = {2026},
  version      = {1.0.0},
  note         = {C++20, Zig, Dear ImGui, SDL2, libssh2, libvterm, SQLite}
}
```

When redistributing binaries or modified sources, retain `LICENSE`, `NOTICE`, and the license texts under `third_party/licenses/`.
