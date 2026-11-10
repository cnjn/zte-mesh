#!/usr/bin/env python3
"""mreplay.py: run the shim's Controller service on the (mainline OpenWrt) router against the REAL child and capture the cold join,
for comparison with the vendor controller's cold join (see docs/research/REPLAY_RESULTS.md). Run it on the mac (python3.9 is enough).

Everything on the router lives in tmpfs (/tmp/zte-mesh-svc, /run/zte-mesh, /tmp/zmc) and is removed again; nothing is installed.
The sandbox wire is attached to the LAN bridge, so the real daemon transmits real IEEE 1905 frames: it can reconfigure real mesh nodes.
Hence the same station guard as dev-service-smoke.py: every station in the bridge fdb must be listed in --expect-macs.
The service config contains the Wi-Fi key: keep that file 0600 outside any repository and delete it afterwards.
"""
import argparse, json, re, shlex, subprocess, sys, time
from pathlib import Path

ROOT = "/tmp/zte-mesh-svc"
CAP = "/tmp/zmc"
CHILD_MAC = "d4:b7:09:0d:77:30"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="root@192.168.5.1")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--bridge", default="br-lan")
    ap.add_argument("--archive", type=Path, default=Path("/Volumes/code/sr1010-mesh/out/zte-mesh-svctest.tar.gz"))
    ap.add_argument("--config", type=Path, required=True, help="service config (key=value, 0600, contains the Wi-Fi key)")
    ap.add_argument("--expect-macs", required=True, help="comma list of every station allowed on the LAN while the controller runs")
    ap.add_argument("--db-profile", type=Path, help="optional /run/zte-mesh/db-profile (see mkprofile.py): vendor baseline values for the policy tables")
    ap.add_argument("--al-last", default="", help="last byte (hex) of the controller AL MAC 02:00:00:00:19:XX; use a new value per run so the child treats it as a new controller")
    ap.add_argument("--identity", default="ZTE,ZXSLC SR1010,8000", help="manufacturer,model,market sign (real-child path)")
    ap.add_argument("--seconds", type=int, default=120, help="observation time after the trigger")
    ap.add_argument("--trigger", default="auto", help="auto = admin down/up of the child's port; port:<ifname>; manual = wait for a physical replug; none")
    ap.add_argument("--swap", action="append", default=[], metavar="SECONDS:FILE",
                    help="replace the running service's config with FILE SECONDS after the trigger (repeatable; same 0600 rule as --config). "
                         "Written to a temp file inside /run/zte-mesh and renamed over config, so the service never reads a half-written file")
    ap.add_argument("--action", action="append", default=[], metavar="SECONDS:REQUEST",
                    help="after the trigger, SECONDS later, hand the service one node action request, e.g. 90:'light d4:b7:09:0d:77:30 0' or "
                         "'150:reboot d4:b7:09:0d:77:30' (repeatable). Written to /run/zte-mesh/actions/<n>.act; the result is in state.json/service.log")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_.@-]+", a.host) or not re.fullmatch(r"[a-zA-Z0-9_.-]+", a.bridge):
        ap.error("unsupported host or bridge name")
    if a.config.stat().st_mode & 0o077:
        sys.exit("refusing: config file must be mode 0600 (it holds the Wi-Fi key)")
    swaps = []
    for item in a.swap:
        sec, sep, name = item.partition(":")
        try:
            swaps.append((float(sec), Path(name)))
        except ValueError:
            ap.error("--swap needs SECONDS:FILE")
        if not sep or not swaps[-1][1].is_file() or swaps[-1][1].stat().st_mode & 0o077:
            sys.exit("refusing: --swap file missing or not mode 0600 (it holds the Wi-Fi key): " + item)
    swaps.sort(key=lambda x: x[0])
    actions = []
    for item in a.action:
        sec, sep, text = item.partition(":")
        try:
            actions.append((float(sec), text))
        except ValueError:
            ap.error("--action needs SECONDS:REQUEST")
        if not sep or not re.fullmatch(r"(reboot [0-9a-f:]{17}|light [0-9a-f:]{17} [01])", text):
            ap.error("--action request must be 'reboot <mac>' or 'light <mac> 0|1': " + item)
    events = sorted([(sec, "swap", name) for sec, name in swaps] + [(sec, "action", text) for sec, text in actions], key=lambda e: e[0])
    manu, model, market = (a.identity.split(",") + ["", "", ""])[:3]
    ssh = ["ssh", "-p", str(a.port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", a.host]
    a.out.mkdir(parents=True, exist_ok=True)

    def remote(cmd, timeout=90, stdin=None, binary=False):
        return subprocess.run(ssh + [cmd], capture_output=True, text=not binary, timeout=timeout, stdin=stdin)

    def log(msg):
        print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

    snapshot = ("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'IPC\\n'; "
                "cat /proc/sysvipc/shm /proc/sysvipc/sem /proc/sysvipc/msg")
    before = remote(snapshot)
    if before.returncode or remote(f"test ! -e {ROOT} && test ! -e /run/zte-mesh && test ! -e {CAP}").returncode:
        sys.exit("router not clean or snapshot failed; refusing")
    fdb = remote(f"bridge fdb show br {a.bridge} | grep -v -e permanent -e ' self '")
    present, child_port = set(), None
    for line in fdb.stdout.splitlines():
        m = re.match(r"([0-9a-fA-F:]{17}) dev (\S+)", line)
        if m:
            present.add(m.group(1).lower())
            if m.group(1).lower() == CHILD_MAC:
                child_port = m.group(2)
    allowed = {m.strip().lower() for m in a.expect_macs.split(",") if m.strip()}
    if fdb.returncode or not allowed or present - allowed:
        sys.exit(f"refusing to attach: LAN stations {sorted(present - allowed)} are not in --expect-macs")
    log(f"stations on the LAN: {sorted(present)}; child port: {child_port}")
    if not child_port and a.trigger in ("auto",):
        sys.exit("child not seen in the bridge fdb (cannot pick a port to flap); use --trigger manual")
    report = {"host": a.host, "stations": sorted(present), "child_port": child_port}
    try:
        with a.archive.open("rb") as stream:
            r = remote(f"mkdir -m 700 {ROOT} && tar -xz -C {ROOT} && cd {ROOT} && sha256sum -c SHA256SUMS >/dev/null", timeout=180, stdin=stream)
        if r.returncode:
            sys.exit("upload failed: " + r.stderr[-300:])
        r = subprocess.run(ssh + ["mkdir -m 700 /run/zte-mesh && umask 077 && cat > /run/zte-mesh/config"],
                           input=a.config.read_text(), capture_output=True, text=True, timeout=30)
        if r.returncode:
            sys.exit("config write failed")
        if a.db_profile:
            r = subprocess.run(ssh + ["umask 077 && cat > /run/zte-mesh/db-profile"], input=a.db_profile.read_text(),
                               capture_output=True, text=True, timeout=30)
            if r.returncode:
                sys.exit("db-profile write failed")
        port = child_port or a.trigger.split(":", 1)[-1]
        remote(f"mkdir -m 700 {CAP}")
        cap = lambda iface, name: (f"(setsid tcpdump -i {iface} -s 0 -nn -U -w {CAP}/{name}.pcap -c 60000 "
                                   f"'ether proto 0x893a or ether proto 0x88cc' > {CAP}/{name}.err 2>&1 &)")
        remote(cap(a.bridge, "br"))
        if child_port:
            remote(cap(child_port, "port"))
        remote(f"(setsid sh -c 'i=0; while [ \"$(cat /sys/class/net/zmesh0/operstate 2>/dev/null)\" != up ] && [ $i -lt 120 ]; do sleep 0.5; i=$((i+1)); done; "
               f"tcpdump -i zmesh0 -s 0 -nn -U -w {CAP}/zmesh.pcap -c 60000 \"ether proto 0x893a\" > {CAP}/zmesh.err 2>&1' > /dev/null 2>&1 &)")
        env = (f"env ZTE_MESH_REAL_MANUFACTURER={shlex.quote(manu)} ZTE_MESH_REAL_MODEL={shlex.quote(model)} "
               f"ZTE_MESH_REAL_MARKET_SIGN={shlex.quote(market)}") if manu else ""
        if a.al_last:
            env = (env or "env") + f" ZTE_MESH_AL_LAST={shlex.quote(a.al_last)}"
        start = (f"cd {ROOT} && (setsid {env} ./ns-run {ROOT}/root /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe "
                 f"--controller-service > {ROOT}/service.log 2>&1 & echo $! > {ROOT}/ns-run.pid)")
        if remote(start).returncode:
            sys.exit("start failed")
        remote(f"(setsid sh {ROOT}/attach {a.bridge} > {ROOT}/attach.log 2>&1 & echo $! > {ROOT}/attach.pid)")
        t0 = time.time()
        while time.time() - t0 < 60 and remote("test -f /run/zte-mesh/state.json").returncode:
            time.sleep(1)
        log("controller service up (state.json present)" if time.time() - t0 < 60 else "WARNING: no state.json after 60 s")
        time.sleep(12)
        if a.trigger == "manual":
            log(">>> REPLUG THE CHILD'S CABLE NOW (unplug ~5 s, plug back) <<<")
        elif a.trigger != "none":
            log(f"trigger: admin down/up of {port}")
            remote(f"ip link set {port} down; sleep 6; ip link set {port} up")
        t_trigger = time.time()
        for n, (sec, kind, what) in enumerate(events):
            delay = t_trigger + sec - time.time()
            if delay > 0:
                time.sleep(delay)
            if kind == "action":
                r = subprocess.run(ssh + [f"umask 077 && cat > /run/zte-mesh/actions/a{n}.act.tmp && mv /run/zte-mesh/actions/a{n}.act.tmp /run/zte-mesh/actions/a{n}.act && date +%s"],
                                   input=what + "\n", capture_output=True, text=True, timeout=30)
                log("node action '%s' at +%.0f s: rc=%d router_epoch=%s" % (what, time.time() - t_trigger, r.returncode, r.stdout.strip()))
                report.setdefault("actions", []).append({"after": sec, "request": what, "rc": r.returncode, "router_epoch": r.stdout.strip()})
                continue
            name = what
            r = subprocess.run(ssh + ["umask 077 && cat > /run/zte-mesh/.config.swap && mv /run/zte-mesh/.config.swap /run/zte-mesh/config && date +%s"],
                               input=name.read_text(), capture_output=True, text=True, timeout=30)
            log("config swap %s at +%.0f s: rc=%d router_epoch=%s" % (name.name, time.time() - t_trigger, r.returncode, r.stdout.strip()))
            report.setdefault("swaps", []).append({"after": sec, "file": name.name, "rc": r.returncode, "router_epoch": r.stdout.strip()})
        rest = t_trigger + a.seconds - time.time()
        if rest > 0:
            time.sleep(rest)
        log("collecting")
        for f in ("br", "port", "zmesh"):
            r = remote(f"cat {CAP}/{f}.pcap 2>/dev/null", binary=True, timeout=120)
            if r.stdout:
                (a.out / f"{f}.pcap").write_bytes(r.stdout)
                log(f"  {f}.pcap: {len(r.stdout)} bytes")
        (a.out / "service.log").write_text(remote(f"cat {ROOT}/service.log").stdout)
        st = remote("cat /run/zte-mesh/state.json 2>/dev/null").stdout
        (a.out / "state.json").write_text(st)
        try:
            report["state"] = json.loads(st)
        except ValueError:
            report["state"] = None
        report["service_log_tail"] = [l for l in (a.out / "service.log").read_text().splitlines() if "ZTE_MESH_SERVICE" in l or "ZTE_CONTROLLER" in l][-12:]
        report["trigger"] = a.trigger
        report["observed_seconds"] = round(time.time() - t_trigger)
    finally:
        # the router has no pkill (busybox without it): the captures of every earlier run kept running, each holding a deleted
        # tmpfs file (about 130 MB of RAM after 30 runs). Kill by command line, only our own (capture files under {CAP}).
        remote("for p in $(ps w | grep '[t]cpdump -i' | grep '" + CAP + "/' | awk '{print $1}'); do kill $p; done; true")
        remote(f"kill $(cat {ROOT}/attach.pid) 2>/dev/null; kill -TERM $(cat {ROOT}/ns-run.pid) 2>/dev/null; sleep 2; "
               f"kill -KILL $(cat {ROOT}/ns-run.pid) 2>/dev/null; ip link del zmesh0 2>/dev/null; ip link del lab-peer 2>/dev/null; true")
        remote(f"rm -rf {ROOT} /run/zte-mesh {CAP}")
        after = remote(snapshot)
        report["host_state_unchanged"] = before.stdout == after.stdout
    (a.out / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
