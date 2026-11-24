#!/usr/bin/env python3
"""Run a JianSSH pause regression using an ephemeral, localhost-only SSH server."""
import hashlib
import logging
import os
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import paramiko


class Handle(paramiko.SFTPHandle):
    def __init__(self, fd, flags, delay):
        super().__init__(flags)
        self.file = os.fdopen(fd, "wb" if flags & os.O_WRONLY else "rb", buffering=0)
        self.writefile = self.readfile = self.file
        self.delay = delay

    def stat(self):
        return paramiko.SFTPAttributes.from_stat(os.fstat(self.file.fileno()))

    def read(self, offset, size):
        time.sleep(self.delay)  # Force multiple network pump passes per file.
        return super().read(offset, size)

    def write(self, offset, data):
        time.sleep(self.delay)
        return super().write(offset, data)


class Files(paramiko.SFTPServerInterface):
    def __init__(self, server, root, opens):
        super().__init__(server)
        self.root, self.opens = root, opens

    def path(self, path):
        # This fixture exports flat files only, never arbitrary host paths.
        name = path.removeprefix("/")
        if not name or name in (".", "..") or "/" in name:
            raise PermissionError(13, "Not a fixture file")
        return self.root / name

    def open(self, path, flags, attr):
        try:
            fd = os.open(self.path(path), flags | os.O_NOFOLLOW, 0o600)
            self.opens.append((path, bool(flags & os.O_WRONLY)))
            return Handle(fd, flags, 0.35 if path == "/slow" else 0.015)
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def stat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(self.path(path).stat())
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    lstat = stat

    def rename(self, old, new):
        try:
            # SFTP v3 no-replace publish.
            os.link(self.path(old), self.path(new))
            self.path(old).unlink()
            return paramiko.SFTP_OK
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def remove(self, path):
        try:
            self.path(path).unlink()
            return paramiko.SFTP_OK
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)


class Peer(paramiko.ServerInterface):
    def get_allowed_auths(self, username):
        return "password"

    def check_auth_password(self, username, password):
        if (username, password) == ("fixture", "pause-test"):
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, *args):
        return True

    def check_channel_shell_request(self, channel):
        def echo():
            try:
                while data := channel.recv(4096):
                    if b"drop-transport" in data:
                        channel.get_transport().close()
                        break
                    channel.sendall(data)
            except (EOFError, OSError):
                pass
        threading.Thread(target=echo, daemon=True).start()
        return True


def run(client):
    key = paramiko.ECDSAKey.generate()
    stop = threading.Event()
    opens = []
    connections = []
    errors = []
    with tempfile.TemporaryDirectory(prefix="jianssh-pause-") as tmp, socket.socket() as listener:
        root = Path(tmp)
        local, remote = root / "local", root / "remote"
        local.mkdir(mode=0o700)
        remote.mkdir(mode=0o700)
        payload = random.Random(48391).randbytes(512 * 1024 + 137)
        (local / "source").write_bytes(payload)
        (remote / "timed").write_bytes(payload[:128 * 1024 + 19])
        (remote / "slow").write_bytes(payload)
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.1)

        def serve():
            # Test cases are sequential and own a single transport each. Keep
            # channel refs until the transport closes (SFTP + shell + monitor).
            try:
                while not stop.is_set():
                    try:
                        sock, _ = listener.accept()
                    except socket.timeout:
                        continue
                    with paramiko.Transport(sock) as transport:
                        connections.append(transport)
                        transport.add_server_key(key)
                        transport.set_subsystem_handler("sftp", paramiko.SFTPServer, Files, remote, opens)
                        transport.start_server(server=Peer())
                        channels = []
                        while transport.is_active() and not stop.is_set():
                            channel = transport.accept(0.1)
                            if channel is not None:
                                channels.append(channel)
            except Exception as e:
                errors.append(e)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            subprocess.run([client, str(listener.getsockname()[1]), tmp], timeout=90, check=True)
        finally:
            stop.set()
            thread.join(timeout=5)
        if thread.is_alive() or errors:
            raise RuntimeError(f"fixture server did not exit normally: {errors}")
        if len(connections) != 2:
            raise AssertionError(f"expected exactly two SSH transports, got {len(connections)}")
        for path in [remote / "uploaded", local / "downloaded"]:
            if path.read_bytes() != payload:
                raise AssertionError(f"corrupt transfer: {path.name}")
        if (local / "timed-result").read_bytes() != (remote / "timed").read_bytes():
            raise AssertionError("timed resume corrupted payload")
        # No reopening the source file from byte zero or replacing the staged
        # upload on resume. Three /uploaded reads: download, cancel, disconnect.
        # /timed and /slow exercise independent low-level deadline cases.
        writes = [p for p, upload in opens if upload]
        reads = sorted(p for p, upload in opens if not upload)
        if len(writes) != 2 or reads != sorted(["/uploaded"] * 3 + ["/timed", "/slow"]):
            raise AssertionError(f"unexpected SFTP handle opens: {opens}")
        print(f"PASS: same handles; no extra transports; {len(payload)} bytes exact, "
              f"sha256={hashlib.sha256(payload).hexdigest()}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 tools/test_sftp_pause.py build/test_sftp_pause")
    logging.getLogger("paramiko.transport").setLevel(logging.CRITICAL)
    run(str(Path(sys.argv[1]).resolve()))
