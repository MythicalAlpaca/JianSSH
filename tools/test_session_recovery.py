#!/usr/bin/env python3
"""Run the C++ workspace regression against a disposable loopback SSH fixture.

From the project root, in bash:
  bash build.sh linux
  ZIG_GLOBAL_CACHE_DIR="$PWD/build/zig-cache" zig c++ -target x86_64-linux-gnu.2.35 \
    -std=c++20 -pthread -Iinclude -Iinclude/imgui tools/test_session_recovery.cpp \
    build/obj/{workspace,key_file_picker,workspace_controller,terminal,host_service,local_files,credential_service,private_key_file,collector,database,ssh_session,serial_session}.o \
    lib/linux/lib{imgui,vterm,sqlite3,ssh2,mbedtls,mbedx509,mbedcrypto,everest,p256m,z,SDL2}.a \
    build/credentials/x86_64-linux-gnu.2.35/libcredential_crypto.a -ldl -o build/test_session_recovery
  python3 tools/test_session_recovery.py build/test_session_recovery
Requires Paramiko (test-only). No system users, sshd or real passwords needed.
"""
import logging
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

import paramiko


class Peer(paramiko.ServerInterface):
    def __init__(self):
        self.shell = threading.Event()
        self.passwords = []
        self.user = None

    def get_allowed_auths(self, username):
        # Withdraw password auth after the first rejection for this test user.
        if username == "withdraw" and self.passwords:
            return "publickey"
        return "password"

    def check_auth_password(self, username, password):
        self.user = username
        self.passwords.append(password)
        return paramiko.AUTH_SUCCESSFUL if password == "fixture-ok" else paramiko.AUTH_FAILED

    def check_channel_request(self, kind, chanid):
        if kind == "session" and chanid == 0:
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, *args):
        return True

    def check_channel_shell_request(self, channel):
        self.shell.set()
        return True


def main(client):
    key = paramiko.ECDSAKey.generate()
    peers = []
    with socket.socket() as listener, tempfile.TemporaryDirectory(prefix="jianssh-recovery-") as tmp:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.2)
        stop = threading.Event()

        def serve(sock, peer):
            with paramiko.Transport(sock) as transport:
                transport.add_server_key(key)
                transport.start_server(server=peer)
                while transport.is_active() and not stop.is_set():
                    if peer.user == "timeout" and peer.passwords:
                        stop.wait(0.5)  # let the retry prompt appear first
                        return
                    channel = transport.accept(0.1)
                    if not channel:
                        continue
                    if peer.shell.wait(3):
                        # Echo on an ordinary PTY, or simulate loss of this transport.
                        while transport.is_active() and not stop.is_set():
                            data = channel.recv(4096)
                            if not data or b"drop-fixture" in data:
                                return
                            channel.sendall(data)

        def accept():
            while not stop.is_set():
                try:
                    sock, _ = listener.accept()
                except socket.timeout:
                    continue
                peer = Peer()
                peers.append(peer)
                threading.Thread(target=serve, args=(sock, peer), daemon=True).start()

        thread = threading.Thread(target=accept)
        thread.start()
        try:
            subprocess.run([client, str(listener.getsockname()[1]), tmp], check=True, timeout=60)
        finally:
            stop.set()
            thread.join()
    # Three bad/good inputs on the initial transport, not three connections.
    # Duplicating and reconnecting a Never-policy session must NOT silently
    # send the old (or an empty) password before the user has answered.
    expected = [
        ["wrong-1", "wrong-2", "fixture-ok"], ["fixture-ok"], ["fixture-ok"],
        ["wrong-1"], ["wrong-1"], ["wrong-1", "fixture-ok"], ["fixture-ok"],
    ]
    expected.extend([["fixture-ok"] for _ in range(4)])
    assert [p.user for p in peers[-4:]] == ["remember", "remember", "recover", "remember"], "wrong double-click target"
    assert [p.passwords for p in peers] == expected, "unexpected number of transports or password submissions"
    print("Session recovery integration passed")


if __name__ == "__main__":
    logging.getLogger("paramiko.transport").setLevel(logging.CRITICAL)
    main(str(Path(sys.argv[1]).resolve()))
