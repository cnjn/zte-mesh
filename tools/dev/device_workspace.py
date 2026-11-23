"""Bounded /tmp staging with guaranteed cleanup attempts; never sweeps old paths."""
import re
import shlex
import sys
import tarfile
from pathlib import Path

PREFIXES = frozenset(("/tmp/zte-mesh-probe.", "/tmp/zte-mesh-db.", "/tmp/zte-mesh-db.ctrl."))
RESERVE_KIB = 32 * 1024


class DeviceWorkspace:
    def __init__(self, remote, archive: Path, prefix: str):
        if prefix not in PREFIXES:
            raise ValueError("unapproved device staging prefix")
        self.remote = remote
        self.prefix = prefix
        self.directory = None
        self.removed = False
        self.free_kib = None
        with tarfile.open(archive, "r:gz") as tar:
            # Streaming upload stores extracted files, not a second tar copy.
            extracted_kib = sum(((m.size + 4095) // 4096) * 4 for m in tar.getmembers() if m.isfile())
        self.required_kib = extracted_kib + RESERVE_KIB + 512

    def __enter__(self):
        result = self.remote("df -Pk /tmp | awk 'NR == 2 {print $4}'", "tmp-space-before")
        text = result.stdout.strip()
        if result.returncode or not re.fullmatch(r"[0-9]+", text):
            raise RuntimeError("cannot verify device /tmp space; refusing upload")
        self.free_kib = int(text)
        if self.free_kib < self.required_kib:
            raise RuntimeError(f"device /tmp too full: available={self.free_kib} KiB required={self.required_kib} KiB; no upload")
        result = self.remote("umask 077; mktemp -d " + shlex.quote(self.prefix + "XXXXXX"), "mkdir")
        directory = result.stdout.strip()
        if result.returncode or not re.fullmatch(re.escape(self.prefix) + r"[A-Za-z0-9]{6}", directory):
            raise RuntimeError("failed to allocate approved device staging directory")
        self.directory = directory
        return self

    def __exit__(self, exc_type, exc, tb):
        path = shlex.quote(self.directory)
        # On a transport timeout a remote probe might still be running. Do not
        # remove a live sandbox's root or try to kill arbitrary host processes.
        command = (
            "if ps w | grep -E '[m]ultiapd|[d]b-engine-probe|[n]s-run|[q]emu-arm' >/dev/null; "
            "then printf 'cleanup deferred: mesh probe active\\n' >&2; exit 1; fi; "
            f"test ! -L {path} && rm -rf -- {path} && test ! -e {path}"
        )
        try:
            cleanup = self.remote(command, "cleanup")
            self.removed = cleanup.returncode == 0
            if not self.removed:
                raise RuntimeError("device staging cleanup failed/deferred: " + self.directory)
        except Exception as error:
            if exc_type is None:
                raise
            print(f"WARNING: {error}; original experiment error preserved", file=sys.stderr)
        return False
