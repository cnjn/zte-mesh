#!/usr/bin/env python3
"""Developer check of the long-running Controller service on the router.

Everything lives in tmpfs (/tmp/zte-mesh-svc, /run/zte-mesh) and is removed
again; nothing is installed or written to flash. The sandbox's wire is
attached to the real LAN bridge for a bounded observation, so the real daemon
transmits real IEEE 1905 frames on the wired LAN. Pass --no-attach to keep it
fully private. Needs a launcher built with -DSERVICE_ROOT='"/tmp/zte-mesh-svc/root"'.
"""
import argparse
import json
import re
import shlex
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
    parser.add_argument("--bridge", default="br-lan")
    parser.add_argument("--archive", type=Path, default=repo / "out/zte-mesh-svctest.tar.gz")
    parser.add_argument("--seconds", type=int, default=25)
    parser.add_argument("--no-attach", action="store_true")
    parser.add_argument("--extra", default="", help="extra service config lines (';'-separated) for the whole run, e.g. mesh.role=master")
    parser.add_argument("--then", action="append", default=[], metavar="LINES",
                        help="after the mesh.v2 step, replace the extra lines by LINES (';'-separated) and observe 12 s; repeatable")
    parser.add_argument("--pcap", type=Path, help="save every 1905 frame seen on the private lab-peer link (--no-attach only)")
    parser.add_argument("--expect-macs", default="", help="comma list of LAN station MACs allowed to be present; attaching "
                        "the controller to a LAN with any other station is refused (it could reconfigure real mesh nodes)")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_.@-]+", args.host) or not re.fullmatch(r"[a-zA-Z0-9_.-]+", args.bridge):
        parser.error("unsupported host or bridge name")
    ssh = ["ssh", "-p", str(args.port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]

    def remote(command, timeout=60, stdin=None):
        return subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout, stdin=stdin)

    snapshot = ("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'IPC\\n'; "
                "cat /proc/sysvipc/shm /proc/sysvipc/sem /proc/sysvipc/msg")
    before = remote(snapshot)
    if before.returncode or remote(f"test ! -e {ROOT} && test ! -e /run/zte-mesh && ! test -d /sys/class/net/lab-peer").returncode:
        sys.exit("router not clean or snapshot failed; refusing")
    report = {"host": args.host}
    if not args.no_attach:
        fdb = remote(f"bridge fdb show br {args.bridge} | grep -v -e permanent -e ' self ' | awk '{{print $1}}' | sort -u")
        present = {m.lower() for m in fdb.stdout.split() if re.fullmatch(r"[0-9a-fA-F:]{17}", m)}
        allowed = {m.strip().lower() for m in args.expect_macs.split(",") if m.strip()}
        if fdb.returncode or not allowed or present - allowed:
            sys.exit(f"refusing to attach: LAN stations {sorted(present - allowed)} are not in --expect-macs")
        report["lan_stations"] = sorted(present)
    try:
        with args.archive.open("rb") as stream:
            r = remote(f"mkdir -m 700 {ROOT} && tar -xz -C {ROOT} && cd {ROOT} && sha256sum -c SHA256SUMS >/dev/null",
                       timeout=180, stdin=stream)
        if r.returncode:
            sys.exit("upload failed: " + r.stderr[-300:])
        extra = "".join(l + "\n" for l in args.extra.split(";") if l)
        profile = ("version_mode=2\nradio0.enabled=1\nradio0.ssid=lab-2g\nradio0.key=lab-key-24g-only\n"
                   "radio1.enabled=1\nradio1.ssid=lab-5g\nradio1.key=lab-key-5g-only\n")
        base_profile = profile
        profile += extra
        r = subprocess.run(ssh + ["mkdir -m 700 /run/zte-mesh && umask 077 && cat > /run/zte-mesh/config"],
                           input=profile, capture_output=True, text=True, timeout=30)
        if r.returncode:
            sys.exit("config write failed")
        start = (f"cd {ROOT} && (setsid ./ns-run {ROOT}/root /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe "
                 f"--controller-service > {ROOT}/service.log 2>&1 & echo $! > {ROOT}/ns-run.pid)")
        if remote(start).returncode:
            sys.exit("start failed")
        if not args.no_attach:
            attach = (f"(setsid sh {ROOT}/attach {args.bridge} > {ROOT}/attach.log 2>&1 & echo $! > {ROOT}/attach.pid)")
            remote(attach)
        if args.no_attach:
            # Carrier only: the peer end stays outside every bridge, so nothing reaches the LAN.
            remote("i=0; while [ $i -lt 20 ] && [ ! -d /sys/class/net/lab-peer ]; do sleep 1; i=$((i+1)); done; "
                   "ip link set dev lab-peer up")
            if args.pcap:
                remote("(setsid tcpdump -i lab-peer -s 0 -nn -U -w /tmp/zte-mesh-svc/lab.pcap 'ether proto 0x893a' "
                       "> /tmp/zte-mesh-svc/lab.err 2>&1 &)")
        capture_if = "lab-peer" if args.no_attach else args.bridge
        time.sleep(args.seconds // 2)
        wire = remote(f"ls /sys/class/net/zmesh0/master 2>/dev/null && echo attached; "
                      f"bridge link show 2>/dev/null | grep -c zmesh0; ip -br link show zmesh0 2>&1")
        capture = remote(f"timeout 1 true 2>/dev/null; tcpdump -i {capture_if} -nn -e -c 12 -l ether proto 0x893a 2>&1 & "
                         f"p=$!; sleep {max(args.seconds // 2, 6)}; kill $p 2>/dev/null; wait $p 2>/dev/null; true",
                         timeout=args.seconds + 30)
        state = remote("cat /run/zte-mesh/state.json 2>/dev/null")
        log = remote(f"cat {ROOT}/service.log")
        report["wire"] = wire.stdout.strip()
        report["capture"] = [re.sub(r"^\S+ ", "", l)[:150] for l in capture.stdout.splitlines() if "0x893a" in l.lower() or "ethertype" in l.lower()][:12]
        try:
            report["state"] = json.loads(state.stdout)
        except ValueError:
            report["state"] = None
        report["service_log"] = [l for l in log.stdout.splitlines() if "ZTE_MESH_SERVICE" in l or "ZTE_DB_SANDBOX" in l or "ZTE_CONTROLLER_STARTUP" in l][-14:]
        # Change the Wi-Fi profile while it runs: the controller must apply it and trigger the renew.
        profile2 = profile.replace("lab-2g", "lab-2g-b")
        base2 = base_profile.replace("lab-2g", "lab-2g-b")
        subprocess.run(ssh + ["umask 077 && cat > /run/zte-mesh/config.new && mv /run/zte-mesh/config.new /run/zte-mesh/config"],
                       input=profile2, capture_output=True, text=True, timeout=30)
        time.sleep(4)
        log2 = remote(f"cat {ROOT}/service.log")
        report["after_profile_change"] = [l for l in log2.stdout.splitlines() if "ZTE_MESH_SERVICE" in l][-6:]
        report["dir_after_change"] = remote("ls -la --full-time /run/zte-mesh; ip -d link show lab-peer 2>&1 | head -3").stdout.splitlines()[:12]
        report["log_tail"] = [l[:200] for l in log2.stdout.splitlines()[-8:]]
        report["log_head"] = [l[:200] for l in log2.stdout.splitlines() if "ZTE_MESH_SHIM" not in l][:40]
        report["log_lines"] = len(log2.stdout.splitlines())
        report["trace_counts"] = {k: len(re.findall(k, log2.stdout)) for k in ("tx1905", "rx1905", "ioctl hwaddr fail", "get interface")}
        report["tx_after_change"] = sorted(set(int(m) for m in re.findall(r" tx1905 cmdu=(\d+) ", log2.stdout)))
        # Toggle the self-networking V2 switch: the service writes P1905ComCfg.Enable and respawns the daemon on the same
        # database (the factory restarts multiapd for it); the service must keep running with a new daemon pid.
        before_state = report["state"] or {}
        subprocess.run(ssh + ["umask 077 && cat > /run/zte-mesh/config.new && mv /run/zte-mesh/config.new /run/zte-mesh/config"],
                       input=profile2 + "mesh.v2=1\n", capture_output=True, text=True, timeout=30)
        time.sleep(10)
        log3 = remote(f"cat {ROOT}/service.log")
        state3 = remote("cat /run/zte-mesh/state.json 2>/dev/null")
        report["after_v2_toggle"] = [l for l in log3.stdout.splitlines() if "v2_update" in l or "daemon_respawn" in l]
        try:
            after_state = json.loads(state3.stdout)
        except ValueError:
            after_state = {}
        report["v2_toggle_ok"] = bool(after_state.get("running") and after_state.get("daemon_restarts") == 1 and
                                      after_state.get("daemon_pid") and after_state.get("daemon_pid") != before_state.get("daemon_pid") and
                                      not after_state.get("restart_required"))
        report["tx_after_v2_toggle"] = len(re.findall(r" tx1905 cmdu=", log3.stdout)) - len(re.findall(r" tx1905 cmdu=", log2.stdout))
        report["cfg_lines"] = [l for l in log3.stdout.splitlines() if "ZTE_MESH_SERVICE cfg " in l]
        # Node actions (reboot / LED of a child): the lab has no child, so well-formed requests are answered -4 (topology
        # not ready) and malformed ones -2; nothing may reach the wire. A request file is consumed (deleted) either way.
        for name, text in (("lab1", "reboot 02:00:00:00:00:01\n"), ("lab2", "light 02:00:00:00:00:01 0\n"),
                           ("lab3", "reboot nonsense\n"), ("lab4", "light 02:00:00:00:00:01 7\n"), ("lab5", "reboot 00:00:00:00:00:00\n")):
            subprocess.run(ssh + [f"umask 077 && cat > /run/zte-mesh/actions/{name}.act"], input=text, capture_output=True, text=True, timeout=30)
        time.sleep(5)
        state_a = remote("cat /run/zte-mesh/state.json 2>/dev/null")
        try:
            report["actions"] = {a["id"]: a["status"] for a in json.loads(state_a.stdout).get("actions", [])}
        except ValueError:
            report["actions"] = None
        report["actions_ok"] = report["actions"] == {"lab1": -4, "lab2": -4, "lab3": -2, "lab4": -2, "lab5": -2}
        report["actions_left"] = remote("ls /run/zte-mesh/actions 2>&1").stdout.split()
        report["steps"] = []
        for lines in args.then:
            # "~key" in a step drops that key from the base profile (e.g. ~radio1.ssid = let the service derive the 5G name)
            drop = {l[1:] for l in lines.split(";") if l.startswith("~")}
            step = "".join(l + "\n" for l in lines.split(";") if l and not l.startswith("~"))
            kept = "".join(l + "\n" for l in base2.splitlines() if l.split("=", 1)[0] not in drop)
            subprocess.run(ssh + ["umask 077 && cat > /run/zte-mesh/config.new && mv /run/zte-mesh/config.new /run/zte-mesh/config"],
                           input=kept + step + "mesh.v2=1\n", capture_output=True, text=True, timeout=30)
            time.sleep(12)
            logn = remote(f"cat {ROOT}/service.log")
            report["steps"].append({"lines": lines,
                                    "log": [l for l in logn.stdout.splitlines() if any(k in l for k in
                                            ("role_update", "role_key_changed", "daemon_respawn", "ZTE_MESH_SERVICE cfg ", "restart_required", "config=rejected", "slot_add", "slot_remove", "slot_update", "wlan_setting_update", "profile_update", "acl_update"))]})
        if args.pcap:
            cap = subprocess.run(ssh + [f"cat {ROOT}/lab.pcap 2>/dev/null"], capture_output=True, timeout=60)
            args.pcap.write_bytes(cap.stdout)
            report["pcap_bytes"] = len(cap.stdout)
    finally:
        # the router has no pkill: end our own capture by command line
        remote("for p in $(ps w | grep '[t]cpdump -i lab-peer' | awk '{print $1}'); do kill $p; done; true")
        remote(f"kill $(cat {ROOT}/attach.pid) 2>/dev/null; kill -TERM $(cat {ROOT}/ns-run.pid) 2>/dev/null; sleep 2; "
               f"kill -KILL $(cat {ROOT}/ns-run.pid) 2>/dev/null; ip link del zmesh0 2>/dev/null; ip link del lab-peer 2>/dev/null; true")
        stop_log = remote(f"tail -3 {ROOT}/service.log 2>/dev/null")
        report["stop_log"] = stop_log.stdout.strip().splitlines()[-3:]
        remote(f"rm -rf {ROOT} /run/zte-mesh")
        after = remote(snapshot)
        report["host_state_unchanged"] = before.stdout == after.stdout
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
