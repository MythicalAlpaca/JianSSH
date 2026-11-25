#!/usr/bin/env python3
"""Resolve and optionally embed the JianSSH build version."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
FALLBACK = "1.00.00"
VERSION_RE = re.compile(r"[1-9][0-9]{0,4}\.[0-9]{2}\.[0-9]{2}")


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=False,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def format_version(count):
    major, remainder = divmod(count, 10_000)
    minor, patch = divmod(remainder, 100)
    return f"{major + 1}.{minor:02d}.{patch:02d}"


def project_version(root=PROJECT, override=None):
    if override is not None:
        return override
    reason = "no complete Git history for this project"
    try:
        shallow = git(root, "rev-parse", "--is-shallow-repository")
        # A source export may sit inside another repository; require this
        # project's build entry in the committed tree before counting it.
        if shallow.returncode == 0 and shallow.stdout.strip() == "false" and (
            git(root, "cat-file", "-e", "HEAD:./build.zig").returncode == 0
        ):
            history = git(root, "rev-list", "--first-parent", "--full-history", "--count", "HEAD", "--", ".")
            if history.returncode != 0:
                raise ValueError(history.stderr.strip() or "could not count JianSSH history")
            return format_version(int(history.stdout))
        if shallow.returncode == 0 and shallow.stdout.strip() == "true":
            reason = "shallow Git checkout (fetch the full history or set JIANSSH_VERSION)"
    except FileNotFoundError:
        reason = "Git is not installed"
    print(f"JianSSH version: {reason}; using {FALLBACK}", file=sys.stderr)
    return FALLBACK


def version_parts(version):
    if not VERSION_RE.fullmatch(version):
        raise ValueError("JIANSSH_VERSION must be x.xx.xx, e.g. 1.00.09")
    parts = tuple(map(int, version.split(".")))
    if parts[0] > 65535:
        raise ValueError("JIANSSH_VERSION major exceeds the Windows version resource limit (65535)")
    return parts


def write_header(path, version, parts):
    # Use integer decimal fields in the PE resource (08/09 are not octal).
    fields = ",".join(map(str, (*parts, 0)))
    content = ("#ifndef JIANSSH_GENERATED_VERSION_H\n"
               "#define JIANSSH_GENERATED_VERSION_H\n"
               f'#define JIANSSH_VERSION_STRING "{version}"\n'
               f"#define JIANSSH_VERSION_TUPLE {fields}\n"
               "#endif\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_text(encoding="ascii") != content:
        path.write_text(content, encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, help="write a C/C++ and Windows RC compatible header")
    args = parser.parse_args()
    try:
        version = project_version(override=os.environ.get("JIANSSH_VERSION"))
        parts = version_parts(version)
        if args.header is not None:
            write_header(args.header, version, parts)
    except (OSError, ValueError) as error:
        parser.exit(1, f"JianSSH version: {error}\n")
    print(version)


if __name__ == "__main__":
    main()
