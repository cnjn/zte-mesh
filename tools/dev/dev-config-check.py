#!/usr/bin/env python3
"""Feed service configuration files to the private lab controller service on the router and report which ones it
accepts.

Every file of DIR (sorted by name) becomes /run/zte-mesh/config in turn; the first one starts the service, the next ones
replace it while it runs. A file is "accepted" when the service logs no 'config=rejected' within the wait after the
change (restart_required lines for keys that appear are fine: that is an accepted file). Like dev-service-smoke.py,
everything lives in tmpfs, the wire stays private (--no-attach equivalent) and nothing is installed.
Needs the same archive as dev-service-smoke.py. The files may hold Wi-Fi keys: they are read only from DIR.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = "/tmp/zte-mesh-svc"


def main():
    repo = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--host", default="root@192.168.5.1")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--archive", type=Path, default=repo / "out/zte-mesh-svctest.tar.gz")
    parser.add_argument("--wait", type=int, default=5, help="seconds to watch the log after each replacement")
    parser.add_argument("--expect-rejected", default="", help="comma list of file names that must be rejected")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_.@-]+", args.host):
        parser.error("unsupported host name")
    files = sorted(p for p in args.directory.iterdir() if p.is_file())
    if not files:
        sys.exit("no configuration files")
    expect_rejected = {n for n in args.expect_rejected.split(",") if n}
    ssh = ["ssh", "-p", str(args.port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]

    def remote(command, timeout=60, stdin=None):
        return subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout, stdin=stdin)

    def put(path, text, name="config"):
        r = subprocess.run(ssh + [f"umask 077 && cat > {path}.new && mv {path}.new {path}"], input=text,
                           capture_output=True, text=True, timeout=30)
        if r.returncode:
            sys.exit("config write failed")

    snapshot = ("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'IPC\\n'; "
                "cat /proc/sysvipc/shm /proc/sysvipc/sem /proc/sysvipc/msg")
    before = remote(snapshot)
    if before.returncode or remote(f"test ! -e {ROOT} && test ! -e /run/zte-mesh && ! test -d /sys/class/net/lab-peer").returncode:
        sys.exit("router not clean or snapshot failed; refusing")
    report = {"files": {}}
    try:
        with args.archive.open("rb") as stream:
            r = remote(f"mkdir -m 700 {ROOT} && tar -xz -C {ROOT} && cd {ROOT} && sha256sum -c SHA256SUMS >/dev/null",
                       timeout=180, stdin=stream)
        if r.returncode:
            sys.exit("upload failed: " + r.stderr[-300:])
        if subprocess.run(ssh + ["mkdir -m 700 /run/zte-mesh && umask 077 && cat > /run/zte-mesh/config"],
                          input=files[0].read_text(), capture_output=True, text=True, timeout=30).returncode:
            sys.exit("config write failed")
        start = (f"cd {ROOT} && (setsid ./ns-run {ROOT}/root /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe "
                 f"--controller-service > {ROOT}/service.log 2>&1 & echo $! > {ROOT}/ns-run.pid)")
        if remote(start).returncode:
            sys.exit("start failed")
        remote("i=0; while [ $i -lt 20 ] && [ ! -d /sys/class/net/lab-peer ]; do sleep 1; i=$((i+1)); done; ip link set dev lab-peer up")

        def rejected():
            return len(re.findall("config=rejected", remote(f"cat {ROOT}/service.log").stdout))

        def running():
            return "running=1" in remote(f"cat {ROOT}/service.log").stdout

        deadline = time.time() + 90
        while time.time() < deadline and not running() and rejected() == 0:
            time.sleep(3)
        report["files"][files[0].name] = "rejected" if rejected() else ("accepted" if running() else "no-start")
        count = rejected()
        for path in files[1:]:
            put("/run/zte-mesh/config", path.read_text())
            time.sleep(args.wait)
            now = rejected()
            report["files"][path.name] = "rejected" if now > count else "accepted"
            count = now
        alive = remote(f"tail -2 {ROOT}/service.log").stdout.strip().splitlines()
        report["tail"] = alive[-2:]
    finally:
        remote(f"kill -TERM $(cat {ROOT}/ns-run.pid) 2>/dev/null; sleep 2; kill -KILL $(cat {ROOT}/ns-run.pid) 2>/dev/null; "
               "ip link del zmesh0 2>/dev/null; ip link del lab-peer 2>/dev/null; true")
        remote(f"rm -rf {ROOT} /run/zte-mesh")
        after = remote(snapshot)
        report["host_state_unchanged"] = before.stdout == after.stdout
    bad = [n for n, v in report["files"].items()
           if (v == "rejected") != (n in expect_rejected)]
    report["unexpected"] = bad
    print(json.dumps(report, indent=2, ensure_ascii=False))
    sys.exit(1 if bad or not report.get("host_state_unchanged") else 0)


if __name__ == "__main__":
    main()
