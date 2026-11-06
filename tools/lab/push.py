#!/usr/bin/env python3
"""Mirror the working tree to the lab VM so it can be built there.

    LAB_HOST=… LAB_USER=… LAB_PASS=… python3 tools/lab/push.py

Packs the repository (minus `out`, `evidence`, `.local`, `.git`), uploads it
and unpacks over `$LAB_DIR` (default `~/zte-mesh`).  Unpacking over the previous
tree is deliberate: it keeps `out/` on the VM, which is expensive to rebuild, and
`make` only cares about source mtimes, which the upload always refreshes.

`.local/` is excluded because it is the machine-local scratch dir that holds the lab
password table (`lab-env.md`) and one-off CI tokens: `pack()` walks the working tree,
not `git ls-files`, so without this entry those secrets are copied onto the build VM
and sit there unread by anything.  Exclusion by directory name only stops new copies --
an already-uploaded `.local/` stays on the VM until it is removed there.

A Windows checkout with core.autocrlf=true has CRLF in the working tree even
though the repository stores LF.  Shell scripts, Makefiles and the `.init`
scripts break on the target when that is copied verbatim (`trap: EXIT: bad trap`,
`case … in` syntax errors), so text files are normalised to LF on the way out.
Detection is by content (no NUL) with an explicit binary suffix list on top.
"""

from __future__ import annotations

import io
import os
import sys
import tarfile
from pathlib import Path

import paramiko

REPO = Path(__file__).resolve().parents[2]
EXCLUDE = {".git", "out", "evidence", ".local", ".workbuddy", "__pycache__"}
BINARY_SUFFIX = {
    ".gz", ".tar", ".tgz", ".xz", ".zst", ".zip", ".jffs2", ".img",
    ".so", ".o", ".a", ".elf", ".apk", ".bin", ".png", ".jpg", ".jpeg", ".gif", ".pdf",
}


def is_text(path: Path, data: bytes) -> bool:
    if path.suffix.lower() in BINARY_SUFFIX:
        return False
    return b"\x00" not in data


def pack() -> bytes:
    buffer = io.BytesIO()
    count = 0
    normalised = 0
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for path in sorted(REPO.rglob("*")):
            relative = path.relative_to(REPO)
            if any(part in EXCLUDE for part in relative.parts):
                continue
            if not path.is_file():
                continue
            data = path.read_bytes()
            if is_text(path, data):
                fixed = data.replace(b"\r\n", b"\n")
                if fixed != data:
                    normalised += 1
                data = fixed
            info = tarfile.TarInfo(str(relative).replace(os.sep, "/"))
            info.size = len(data)
            info.mode = path.stat().st_mode & 0o777
            info.mtime = int(path.stat().st_mtime)
            archive.addfile(info, io.BytesIO(data))
            count += 1
    if normalised:
        print(f"normalised CRLF -> LF in {normalised} text files")
    print(f"packed {count} files")
    return buffer.getvalue()


def main() -> int:
    host = os.environ.get("LAB_HOST", "")
    user = os.environ.get("LAB_USER", "")
    password = os.environ.get("LAB_PASS") or None
    key = os.environ.get("LAB_KEY") or None
    remote = os.environ.get("LAB_DIR", "zte-mesh")
    if not host or not user:
        print("LAB_HOST and LAB_USER must be set", file=sys.stderr)
        return 2

    payload = pack()
    print(f"{len(payload) / 1e6:.1f} MB")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(hostname=host, username=user, password=password, key_filename=key,
                   look_for_keys=bool(key), allow_agent=False, timeout=15)

    upload = f"/tmp/zte-mesh-src-{os.getpid()}.tgz"
    transport = client.open_sftp()
    transport.putfo(io.BytesIO(payload), upload)
    transport.close()
    print(f"uploaded {upload}")

    command = (
        f"mkdir -p ~/{remote} && tar -xzf {upload} -C ~/{remote} && rm -f {upload} && "
        f"echo synced $(find ~/{remote} -type f | wc -l) files"
    )
    stdin, stdout, stderr = client.exec_command(f"bash -lc '{command}'", timeout=600)
    stdin.close()
    out = stdout.read().decode()
    err = stderr.read().decode()
    status = stdout.channel.recv_exit_status()
    client.close()

    sys.stdout.write(out)
    sys.stderr.write(err)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
