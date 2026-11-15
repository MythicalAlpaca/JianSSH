#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Build each supported desktop target sequentially: the build shares generated
# assets and cache directories, so parallel builds can race.
case "${1:-}" in
  ""|all)
    if [[ $# -gt 0 ]]; then shift; fi
    env -u JIANSSH_PLATFORM zig build "$@"
    env -u JIANSSH_PLATFORM zig build windows "$@"
    ;;
  linux)
    shift
    exec env -u JIANSSH_PLATFORM zig build "$@"
    ;;
  windows)
    shift
    exec env -u JIANSSH_PLATFORM zig build windows "$@"
    ;;
  *)
    # Preserve existing Zig build steps and flags (run, deps-check, etc.).
    exec zig build "$@"
    ;;
esac
