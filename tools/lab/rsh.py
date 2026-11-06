#!/usr/bin/env python3
"""Run a command on the lab VM over SSH.

Credentials come from the environment, never from the repository:

    LAB_HOST=<地址> LAB_USER=<账号> LAB_PASS=<口令> python3 tools/lab/rsh.py 'uname -a'

Optional: LAB_PORT (default 22), LAB_KEY (path to a private key; preferred
once the key is installed, since password auth is only a bootstrap path).

Pass --stdin to forward local standard input to the remote command, which is
how `sudo -S` gets its password without the secret appearing in the command:

    printf '%s\\n' "$LAB_PASS" | python3 tools/lab/rsh.py --stdin 'sudo -S id'

The command is sent verbatim to `bash -lc`, so quoting is the caller's job.
Exit status of the remote command is propagated.
"""

from __future__ import annotations

import os
import sys

import paramiko


def main() -> int:
    argv = sys.argv[1:]
    forward_stdin = "--stdin" in argv
    argv = [a for a in argv if a != "--stdin"]
    if not argv:
        print(__doc__.strip())
        return 2

    host = os.environ.get("LAB_HOST", "")
    user = os.environ.get("LAB_USER", "")
    password = os.environ.get("LAB_PASS") or None
    key = os.environ.get("LAB_KEY") or None
    port = int(os.environ.get("LAB_PORT", "22"))

    if not host or not user:
        print("LAB_HOST and LAB_USER must be set", file=sys.stderr)
        return 2

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        hostname=host,
        port=port,
        username=user,
        password=password,
        key_filename=key,
        look_for_keys=bool(key),
        allow_agent=False,
        timeout=15,
    )

    command = " ".join(argv)
    stdin, stdout, stderr = client.exec_command(f"bash -lc {shquote(command)}", timeout=3600)
    if forward_stdin:
        payload = sys.stdin.buffer.read()
        if payload:
            stdin.write(payload)
            stdin.flush()
    stdin.close()
    for line in stdout:
        sys.stdout.write(line)
    for line in stderr:
        sys.stderr.write(line)
    status = stdout.channel.recv_exit_status()
    client.close()
    return status


def shquote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


if __name__ == "__main__":
    raise SystemExit(main())
