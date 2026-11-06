#!/usr/bin/env python3
"""Lab check of the Wi-Fi schedule and the access-time countdown of the controller service (REPLAY_RESULTS section 16).

Runs the private lab service on the router (tmpfs, wire not attached) with real wall-clock time, so it takes about five minutes:
  * a schedule window (scope all, utc_offset 0) that opens at the next-but-one minute boundary and closes two minutes later:
    the service must switch every main SSID and slots 1-2 off (and leave slot 3 alone), renew both bands, and switch them on
    again when the window closes;
  * slot 2 with access_minutes=1: about 60 s after the start it must be switched off ("access_expired"); switching its
    configured enable off and on again restarts the countdown, so it expires a second time.
Reports the observed service log lines with their router epochs and a pass/fail per expectation.
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
    parser.add_argument("--host", default="root@192.168.5.1")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--archive", type=Path, default=repo / "out/zte-mesh-svctest.tar.gz")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_.@-]+", args.host):
        parser.error("unsupported host name")
    ssh = ["ssh", "-p", str(args.port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]

    def remote(command, timeout=60, stdin=None):
        return subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout, stdin=stdin)

    def put(path, text):
        r = subprocess.run(ssh + [f"umask 077 && cat > {path}.new && mv {path}.new {path}"], input=text, capture_output=True, text=True, timeout=30)
        if r.returncode:
            sys.exit("config write failed")

    snapshot = ("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'IPC\\n'; "
                "cat /proc/sysvipc/shm /proc/sysvipc/sem /proc/sysvipc/msg")
    before = remote(snapshot)
    if before.returncode or remote(f"test ! -e {ROOT} && test ! -e /run/zte-mesh && ! test -d /sys/class/net/lab-peer").returncode:
        sys.exit("router not clean or snapshot failed; refusing")
    now = int(remote("date -u +%s").stdout.strip())
    window_start = (now // 60 + 2) * 60          # next-but-one minute boundary (UTC)
    window_end = window_start + 120
    hm = lambda t: time.strftime("%H:%M", time.gmtime(t))

    def config(slot2_enabled=1):
        return ("version_mode=2\nradio0.enabled=1\nradio0.ssid=lab-2g\nradio0.key=lab-key-24g-only\n"
                "radio1.enabled=1\nradio1.ssid=lab-5g\nradio1.key=lab-key-5g-only\n"
                "radio0.bss1.enabled=1\nradio0.bss1.ssid=lab-g0\nradio0.bss1.key=lab-key-g0\n"
                "radio1.bss1.enabled=1\nradio1.bss1.ssid=lab-g1\nradio1.bss1.key=lab-key-g1\n"
                f"radio0.bss2.enabled={slot2_enabled}\nradio0.bss2.ssid=lab-s0\nradio0.bss2.key=lab-key-s0\nradio0.bss2.access_minutes=1\n"
                "radio1.bss2.enabled=1\nradio1.bss2.ssid=lab-s1\nradio1.bss2.key=lab-key-s1\n"
                "radio1.bss3.enabled=1\nradio1.bss3.ssid=lab-b1\nradio1.bss3.key=lab-key-b1\n"
                f"timer.utc_offset=0\ntimer.1.start={hm(window_start)}\ntimer.1.end={hm(window_end)}\ntimer.1.scope=all\n")

    report = {"window_utc": f"{hm(window_start)}-{hm(window_end)}"}
    try:
        with args.archive.open("rb") as stream:
            r = remote(f"mkdir -m 700 {ROOT} && tar -xz -C {ROOT} && cd {ROOT} && sha256sum -c SHA256SUMS >/dev/null", timeout=180, stdin=stream)
        if r.returncode:
            sys.exit("upload failed: " + r.stderr[-300:])
        if subprocess.run(ssh + ["mkdir -m 700 /run/zte-mesh && umask 077 && cat > /run/zte-mesh/config"], input=config(), capture_output=True,
                          text=True, timeout=30).returncode:
            sys.exit("config write failed")
        start = (f"cd {ROOT} && (setsid ./ns-run {ROOT}/root /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe "
                 f"--controller-service > {ROOT}/service.log 2>&1 & echo $! > {ROOT}/ns-run.pid)")
        if remote(start).returncode:
            sys.exit("start failed")
        remote("i=0; while [ $i -lt 20 ] && [ ! -d /sys/class/net/lab-peer ]; do sleep 1; i=$((i+1)); done; ip link set dev lab-peer up")
        t_start = int(remote("date -u +%s").stdout.strip())
        # wait for the service, then let the countdown of slot 2 expire once, re-arm it, and run past the end of the window
        swapped = False
        deadline = window_end + 100
        while int(remote("date -u +%s").stdout.strip()) < deadline:
            time.sleep(5)
            log = remote(f"cat {ROOT}/service.log").stdout
            if not swapped and "access_expired radio=0 slot=2" in log:
                # re-arm: configured off, then on again (two config passes, a few seconds apart)
                put("/run/zte-mesh/config", config(0))
                time.sleep(6)
                put("/run/zte-mesh/config", config(1))
                swapped = True
        log = remote(f"cat {ROOT}/service.log").stdout
        keep = ("ZTE_MESH_SERVICE sched", "access_expired", "slot_update", "profile_update", "config=rejected", "fail=")
        report["log"] = [l[:150] for l in log.splitlines() if any(k in l for k in keep)]
        state = remote("cat /run/zte-mesh/state.json 2>/dev/null").stdout
        try:
            report["state_sched"] = json.loads(state).get("sched")
        except ValueError:
            report["state_sched"] = None
        lines = report["log"]

        def at(pattern, after=0):
            for i, l in enumerate(lines):
                if i >= after and re.search(pattern, l):
                    return i
            return -1

        checks = {}
        i_open = at(r"sched off_all=1 off_guest=1 epoch=\d+")
        checks["window opens"] = i_open >= 0
        # while the window is open slots 1-2 and the main SSIDs go off, slot 3 never
        slots_off = [l for l in lines[i_open:] if "slot_update" in l and re.search(r"enable=0/0", l)] if i_open >= 0 else []
        checks["slots 1-2 switched off by the window"] = any("slot=1" in l for l in slots_off) and any("slot=2" in l for l in slots_off)
        checks["slot 3 untouched"] = not any("slot=3" in l and "enable=0/0" in l for l in lines)
        i_close = at(r"sched off_all=0 off_guest=0 epoch=\d+", i_open + 1 if i_open >= 0 else 0)
        checks["window closes"] = i_close >= 0
        checks["slots back on after the window"] = i_close >= 0 and any("slot=1" in l and "enable=1/1" in l for l in lines[i_close:])
        checks["access time expired at least once"] = at(r"access_expired radio=0 slot=2 minutes=1") >= 0
        first = at(r"access_expired radio=0 slot=2 minutes=1")
        checks["access time re-armed and expired again"] = first >= 0 and at(r"access_expired radio=0 slot=2 minutes=1", first + 1) >= 0
        checks["no rejected config"] = not any("rejected" in l or "fail=" in l for l in lines)
        report["checks"] = checks
    finally:
        remote(f"kill -TERM $(cat {ROOT}/ns-run.pid) 2>/dev/null; sleep 2; kill -KILL $(cat {ROOT}/ns-run.pid) 2>/dev/null; "
               "ip link del zmesh0 2>/dev/null; ip link del lab-peer 2>/dev/null; true")
        remote(f"rm -rf {ROOT} /run/zte-mesh")
        after = remote(snapshot)
        report["host_state_unchanged"] = before.stdout == after.stdout
    print(json.dumps(report, indent=2, ensure_ascii=False))
    sys.exit(0 if report.get("checks") and all(report["checks"].values()) and report["host_state_unchanged"] else 1)


if __name__ == "__main__":
    main()
