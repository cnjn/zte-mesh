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

## dev/ (the build- and test-time scripts, moved off the repository root on 2026-11-04)

Imported by module name from `openwrt/make-runtime.py` and `tests/`, so they stay in one directory:
`sys.path` gets `tools/dev`, and a script run as `python3 tools/dev/x.py` finds its siblings on its own.

| Script | What it does |
|---|---|
| `prepare.py` | Builds `out/` from the pinned factory JFFS2 (never from loose ELFs): extracts `multiapd`, walks its `DT_NEEDED` closure, records a hash per file in `out/manifest.json`. `--analysis-cspd` additionally extracts `cspd` to `out/analysis/cspd` (0600, not part of the runtime). |
| `jffs2.py` | Reads one regular file out of a little-endian JFFS2 evidence image — deliberately not a general unpacker; bad CRCs and holes are errors. |
| `vendor_elf.py` | Bounded ELF metadata reader for the vendor ARM32 runtime (no execution), plus the GNU symbol-version audit `make-runtime.py` reruns on the finished archive. |
| `smoke.py` | Audits the built shim, lists the factory libraries, then stops at the DB boundary. Called from `build.sh` inside the builder container (`python3 tools/dev/smoke.py --output /artifacts`). |
| `schema.py` | Public `MultiAPCfg` registry plus hash-pinned, data-only constructor verification of `cspd`. `TABLE_DEFS` points at `src/*.def`. |
| `profile_schema.py` | The same for WLAN/identity metadata (`WLANBase`/`WLANCfg`/`WLANSSID`/`WLANPSK`/`DevInfo`); source default values are never read. |
| `startup-schema.py` | Metadata for the views that actually block bootstrap/configuration (`--cspd --libdb --verify src/startup-views.inc`). |
| `cm-profile-evidence.py` | Recovers the read-only CM AP profile copy (2496 bytes, not the 2312-byte `WLANCfg` view) from pinned ARM machine instructions; `Makefile:19` runs it with `--verify src/cm-profile-layout.inc`. |
| `bridge_evidence.py` | Checks the bridge's one mode against the public WLAN enum tables in `src/mesh-bridge.h`, pinned `libcfapi.so` included. |
| `dev-service-smoke.py` | Runs the long-running controller service on the router entirely in tmpfs; `--no-attach` keeps the wire out of `br-lan`, `--expect-macs` is the station guard when it is attached. |
| `dev-config-check.py DIR` | Feeds every file of `DIR` to that private service as `/run/zte-mesh/config` and reports accepted/rejected. |
| `dev-sched-check.py` | Times the schedule windows and the guest access countdown against the real wall clock (~5 minutes). |
| `device-smoke.py` | Deploys a checksummed `/tmp`-only probe over SSH and checks native ARM32 execution; never starts a controller. |
| `device-db-smoke.py` | Real vendor DB server/client experiment inside private namespaces (`ns-run` isolation, no host-IPC fallback). |
| `device-bridge-smoke.py` | Native ARM64 → private ARM32 DB bridge tests with public fixture inputs. |
| `device-controller-smoke.py` | Bounded real `multiapd` startup in private fixture DB/net/mount namespaces (includes CMDU 10); also re-runs the two `--verify` evidence scripts. |
| `device_workspace.py` | Bounded `/tmp` staging with guaranteed cleanup attempts; never sweeps old paths. Shared by the four `device-*.py`. |
| `verify-install.sh` | Runs **on the router** (busybox ash): install a batch of apks, restart rpcd and the service, then read the machine back as `KEY=VALUE` evidence lines plus a final `RESULT=PASS\|FAIL` — the service role, the instance set that role implies, what the Wi-Fi applier did, the hop count, the emulator and launcher answers. Without a directory argument it only reads. `tests/test_verify_install.sh` grades the judgement against stubs; docs/INSTALL.md section 3 is the on-device use. |

## capture/ (vendor study and real-child replay)

| Script | What it does |
|---|---|
| `mreplay.py` | Runs the controller service on the router against the real child (tmpfs only), triggers a cold join, captures both sides; `--swap`, `--action`, `--al-last`, station guard `--expect-macs`. |
| `cmdu.py` | IEEE 1905 CMDU sequence of a pcap with direction (`--tlv` for TLV lists). |
| `frames.py` | **Frame-length and CMDU-type census** straight from the pcap record headers (`--min`, `--cmdu`, `--dir`, `--tlv`, `--vendor`). Written after `awk '{print $NF}'` on `tcpdump -e` output turned a 771-byte M1 into "68 bytes, so the frame never went out" — the text stream is interrupted by hex dump, only the binary header is trustworthy. `--vendor` additionally pairs every ACD Vendor request (subtype `0x0100`/`0x0101`, opcode 0/1/2) with its reply and reports the shortest repeating opcode period — which is how "the queue resets every round, so state 4 is never reached" became visible instead of guessed. |
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
