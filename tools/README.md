# tools/

None of these scripts contains credentials: the child's telnet password comes from `ZTE_TN_PASS`, service configs with Wi-Fi keys are
passed as files that stay outside the repository. Unless noted, run them on the mac (`/Volumes/code/sr1010-mesh`).

## build/

| Script | What it does |
|---|---|
| `container-make.sh TARGET...` | Runs make targets in the offline builder container (`sr1010-mesh-shim-builder`): artifacts relative to `out/` (deleted inside the container first, see docs/DEVELOPMENT.md section 2) or `all`, `database`, `controller`, `test-host`. Keeps make's exit status. |
| `runtime.sh install\|svctest` | Builds the matching launcher and packs `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` (OpenWrt package, committed; root `/usr/lib/zte-mesh/root`) or `out/zte-mesh-svctest.tar.gz` (tmpfs lab runs, root `/tmp/zte-mesh-svc/root`). |

## dev/ (the build- and test-time Python, moved off the repository root on 2026-11-04)

Imported by module name from `openwrt/make-runtime.py`, `tests/` and `tools/lab/`, so they stay in one directory:
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

## lab/ (x86_64 Ubuntu VM: build and run without the mac or the router)

| Script | What it does |
|---|---|
| `rsh.py` | Runs a command on the lab VM over SSH (`LAB_HOST`/`LAB_USER`/`LAB_PASS`, or `LAB_KEY`); `--stdin` forwards standard input, which is how `sudo -S` gets its password without putting it in the command line. |
| `push.py` | Packs the working tree (minus `.git`, `out`, `evidence`, `.local`, `.workbuddy`, `__pycache__`), uploads it and unpacks over `~/zte-mesh`. Normalises CRLF to LF: a Windows checkout has CRLF in the working tree and the shell tests fail on the target otherwise. The exclusion list is what keeps `.local/lab-env.md` (the lab password table) out of the lab VM — `.gitignore` does not apply to a working-tree walk. Unpacking never deletes, so a file removed here survives there. |
| `bootstrap-out.py` | Rebuilds `out/runtime` + `out/manifest.json` from `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` (hash-checked against its `SHA256SUMS` and `prepare.MULTIAPD_SHA256`), for a machine that has no factory JFFS2. The manifest is marked `source=runtime-bundle`, so `prepare.py` still refuses it and packaging cannot silently use it. |
| `build.sh` | `make` wrapper for Ubuntu: adds `-U_FORTIFY_SOURCE` (Ubuntu's gcc defines it by default, Debian's does not) so `-Werror` builds match the audited build. Defaults `ROUTERCC=aarch64-linux-gnu-gcc`; pass `ROUTERCC=cc` for router-side tools that must run on the VM. |
| `mkroot.sh` | Assembles the tmpfs lab root `/tmp/zte-mesh-svc/root` from a built `out/` (same layout as `make-runtime.py`, which refuses a synthetic `out/`). |
| `run-service.sh` | Starts the controller service in the sandbox via `ns-run-svctest`; `--seconds N`, `--config FILE`. Needs root. |
| `emulate.sh` | The same sandbox built by hand with `qemu-arm-static`, so emulator flags (`-strace`) can be passed — `ns-run` goes through binfmt and cannot. |
| `syms.py` | Which object exports/imports a symbol (`python3 tools/lab/syms.py out/runtime/lib/libdb.so --imports --prefix shm`). |
| `rssh.py` | SSH/scp to the three test machines (Xiaomi AX3600 / ImmortalWrt VM / Ubuntu) by password — there is no `sshpass` here and `ssh` cannot be fed one. Accounts and passwords are **not in this tree**: the script reads the table in `.local/lab-env.md` (gitignored), overridable per machine by `ZTE_LAB_USER_<alias>` / `ZTE_LAB_PASS_<alias>`. `--bench` prints arch/kernel/distro, `put`/`get` copy files, `--pty` only for the few commands that need a tty. Three traps are handled in code: dropbear's exec with a pty treats the command as login-shell input (`echo HI` returns 127), AX3600 has no `sftp-server` so it falls back to a `cat` pipe, and **Git Bash rewrites `/tmp/x` into a Windows path** — callers must `export MSYS_NO_PATHCONV=1`. |
| `build-launcher.sh` | Compiles `ns-run` and `mesh-search` for a machine whose CPU the `zte-mesh` apk does not carry — the apk is `PKGARCH:=all` and ships one build per CPU, but a feed built inside a source tree only has that tree's own toolchain, so its launcher carries one architecture and everything else is told so by the entry script. The binary goes into `/usr/libexec/zte-mesh/arch/<cpu>/`, installed with a backup and a pre-restart check. **`-static` is mandatory**: the build host is glibc, the target is musl, and a dynamically linked binary dies with `execve` → ENOENT ("not found") that looks exactly like a missing file. The check greps for the `usage: ns-run` banner, not the exit code, because `--help` legitimately returns 64. |
| `elfdeps.py` | Closure check: every import of an executable traced back to *which* `.so` in a directory supplies it (`--closure BIN LIBDIR`), plus `DT_NEEDED`/`SONAME` per file (`--graph DIR`). Pure stdlib, no binutils. Answers "is this vendor stack self-contained?" — `multiapd`: 225 imports, 220 from the shipped libs, 5 GCC weak symbols, **0 from the kernel**. Results: docs/research/BLOB_SHIM.md section 2. |
| `armcalls.py` | **Call-site finder**: who calls `system`/`popen`/`ioctl`/any PLT symbol, and what string is in `r0` (`armcalls.py BIN system popen --all`). Rebuilds the ARM PLT map from `.rel.plt` GOT slots + the 12-byte lazy stub form, then scans `bl` targets and backtracks `r0` (`ldr r0,[pc,#imm]` / `adr r0` / `mov r0,#imm`). Pure stdlib, no capstone. Pitfall it documents: vendor binaries wrap exec in their own function (`MonitorPcStartProgram`), so scanning libc only gives false negatives — results: BLOB_SHIM.md sections 3.1/3.2. |
| `cmapi-tables.py` | **DB mapping**: which table each `Wlan*Db*` function reads/writes (`--tables`), who touches any table across the whole library set (`--db-users`), and where an API lands with direct vs indirect calls separated (`--api` / `--multiapd-api`). Resolves PIC string addresses via `.rel.dyn` `R_ARM_RELATIVE` GOT addends plus the `ldr =offset; add rX, pc, rX` pair, and only accepts the `r0` seen **at the `dbAPIGetView`/`dbAPISetView` call site**. Results: 34 functions → 17 tables in the E2631 build, 32 → 16 in the SR1010 build, the 30 shared ones byte-identical; 124 functions touch 29 tables, but `multiapd` has only 4 write paths. See BLOB_SHIM.md section 3.3. |
| `unpack_jffs2.py` | Pure-stdlib JFFS2 unpacker for a factory flash image; auto-detects endianness and the inode layout, so the ZTE 68-byte-header variant (`ino`/`version` swapped, extra 4 bytes before `data`, non-zlib CRC) is not silently unpacked into garbage. `--list` first, `-o DIR` to write. Results: docs/research/E2631_ROOTFS.md section 2. |
| `unpack_squashfs.py` | Pure-stdlib squashfs 4.0 reader (gzip/lzma/xz/lz4/zstd) for the `kmodule` partition a JFFS2 root links to; `--list`, `--cat PATH`, `-o DIR`. |
| `ecb-key-scan.py` | AES-128-ECB rootfs key scanner (method from zte-sr1010-research phase 5): JFFS2 erased flash is `0xFF`-filled, so the most frequent 16-byte ciphertext block equals `AES_ECB(key, FF*16)` and each candidate key is tested with one block encryption — no filesystem magic needed. Slides printable-ASCII runs (`--scan`) or every offset (`--full`), or tests one key directly (`--key-ascii`/`--key-hex`); `--decrypt PATH` writes the region once a key is found. Pure stdlib (embedded AES-128, FIPS-197 self-test). Negative result on E2633: docs/research/E2633_FIRMWARE.md section 5.4. |
| `shm-fixed.c` | Probe for fixed-address `shmat()` under qemu-user: `NULL` and `|SHM_REMAP` work, a plain fixed address returns `EINVAL`. This is why `shim.c` retries that call on an emulated run. |

Setup steps, the failure modes behind each script and the current test status: [docs/LAB.md](../docs/LAB.md).

## drivers/

`example-*.py`: test drivers from an early session (channel/power, ACL, ACL ban of a real client), kept as examples. They still point at
that session's scratch paths and the retired `192.168.174.5` forward; adapt them before use and read docs/DEVELOPMENT.md section 5 first.
