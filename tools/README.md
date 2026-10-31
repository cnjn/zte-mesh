# tools/

None of these scripts contains credentials: the child's telnet password comes from `ZTE_TN_PASS`, service configs with Wi-Fi keys are
passed as files that stay outside the repository. Unless noted, run them on the mac (`/Volumes/code/sr1010-mesh`).

## build/

| Script | What it does |
|---|---|
| `container-make.sh TARGET...` | Runs make targets in the offline builder container (`sr1010-mesh-shim-builder`): artifacts relative to `out/` (deleted inside the container first, see docs/DEVELOPMENT.md section 2) or `all`, `database`, `controller`, `test-host`. Keeps make's exit status. |
| `runtime.sh install\|svctest` | Builds the matching launcher and packs `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` (OpenWrt package, committed; root `/usr/lib/zte-mesh/root`) or `out/zte-mesh-svctest.tar.gz` (tmpfs lab runs, root `/tmp/zte-mesh-svc/root`). |
| `bootstrap-out.py` | Rebuilds `out/runtime` + `out/manifest.json` from `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` (hash-checked against its `SHA256SUMS` and `prepare.MULTIAPD_SHA256`), for a machine that has no factory JFFS2 (also used by `build-lab-tools.yml`). The manifest is marked `source=runtime-bundle`, so `prepare.py` still refuses it and packaging cannot silently use it. |
| `po2lmo.py` | Compiles the LuCI `.po` into a `.lmo` without a build tree (same format as LuCI's `po2lmo.c`), for hand-installing the translation; `tests/test_i18n.py` uses it to check every msgid resolves. |

## capture/ (vendor study and real-child replay)

| Script | What it does |
|---|---|
| `mreplay.py` | Runs the controller service on the router against the real child (tmpfs only), triggers a cold join, captures both sides; `--swap`, `--action`, `--al-last`, station guard `--expect-macs`. |
| `cmdu.py` | IEEE 1905 CMDU sequence of a pcap with direction (`--tlv` for TLV lists). |
| `mcompare.py` | Compares two captures (vendor vs shim) frame by frame, optionally without periodic frames. |
| `respawn_report.py` | What the wire shows after each daemon respawn. |
| `mkprofile.py` | Turns a vendor `sendcmd 1 DB p` snapshot into a `db-profile` for the service (policy tables). |
| `snapdb.py` | Diffs two vendor DB snapshots. |
| `vt.sh`, `vfetch.sh`, `mpull.sh`, `mstep.sh` | For a router running the **vendor** firmware: run shell text over telnet, copy a file off it, pull its capture and DB changes, summarise what it did since a time. |

## child/ (the Z506 child, over telnet from the mac)

| Script | What it does |
|---|---|
| `ztetn.py` | Minimal telnet client (default `192.168.5.216:23`). `ZTE_TN_USER` (default root) / `ZTE_TN_PASS`. Commands as arguments or one per line on stdin. |
| `childread.sh` | 8-row `WLANCfg` readback (no keys printed, the dump is deleted on the child). Compare with the baseline in docs/REAL_CHILD.md before and after every run that touches slots. |
| `child-state.sh` | Read-only snapshot: `MultiAPCfg`, `P1905ComCfg`, `map.slave dev`, process list (feed to `ztetn.py` on stdin). |
| `child-acl.sh` | ACL rules/policies in the DB and in the driver. |
| `child-election-flag.sh` | Reads `g_TriggerSelectFlag` from `cspd` memory (role election trigger, FINDINGS section 3). |
| `child-log-follow.sh` | Follows the child log for 160 s. |
| `pcap1905.py` | Tiny IEEE 1905 pcap dumper. |

## probe/ (static analysis of the vendor binaries, no run needed)

| Script | What it does |
|---|---|
| `agent-surface.py` | Answers what a Slave role would actually need: where `slave_init` is gated, which way an M2 is applied (database setters or a `wlan_mgr` event), and whether the setter writes the database before notifying `cspd`. Reads `out/vendor/root/bin/multiapd` and `out/vendor/root/lib/libcmapi.so`; `--trace` prints every direct call, `--elf` takes another build. Results and open questions: docs/research/AGENT_PROBE.md. |

## drivers/

`example-*.py`: test drivers from an early session (channel/power, ACL, ACL ban of a real client), kept as examples. They still point at
that session's scratch paths and the retired `192.168.174.5` forward; adapt them before use and read docs/DEVELOPMENT.md section 5 first.
