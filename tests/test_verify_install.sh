#!/bin/sh
# Host test for tools/lab/verify-install.sh without a router: the seven commands it reads through (ubus / apk / uci /
# iw / ip / logread / date) are the same names the script takes from its own environment, so the stubs answer from
# fixtures and the assertions are about what the script decided.
# The one program it runs by installed path is the launcher entry script, and that one is the real
# files/launcher-dispatch -- only `uname -m` is stubbed, so the cases judge a fixed CPU instead of the test host's.
# Run with the shell OpenWrt uses: busybox ash tests/test_verify_install.sh
#
# What this test guards is the judgement, not the router: which instance set a role implies, which keys of the status
# document are read, what "nothing should have been written here" means on a controller that touches no radio. Those
# are the parts that silently rot when the init or the status call changes.
SCRIPT=${1:-$(dirname "$0")/../tools/lab/verify-install.sh}
[ -f "$SCRIPT" ] || { echo "verify-install.sh not found: $SCRIPT" >&2; exit 1; }
# the real entry script, not a stub of it: the read-back asks the launcher a question (--launcher-check), so the
# suite that pins the answer pins the contract between the two files rather than two independent guesses.
DISPATCH=$(dirname "$0")/../openwrt/zte-mesh/files/launcher-dispatch
[ -f "$DISPATCH" ] || { echo "launcher-dispatch not found: $DISPATCH" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
BIN=$T/bin
FIX=$T/fix
RUN=$T/run
mkdir -p "$BIN" "$FIX" "$RUN"

# The script takes its seven commands and its run directory from the environment precisely so this test can point them
# at the fixtures. Forgetting one of these would make the test read the real machine (or fail on a missing ubus).
UBUS=$BIN/ubus
APK=$BIN/apk
UCI=$BIN/uci
IW=$BIN/iw
IP=$BIN/ip
LOGREAD=$BIN/logread
DATE=$BIN/date
INIT_D=$T/init.d
ROOTFS=$T/rootfs
RESTART_WAIT=0
# the profile smoke is the one that waits; the fixtures answer immediately, so no case pays for a settle
SSID_WAIT=0
SSID_POLL=1
# ... and neither does the peer re-read: one snapshot per run unless a case says otherwise (and stubs `sleep`, which is
# how the case that watches a frame arrive between two snapshots stays deterministic).
METRICS_READS=1
METRICS_READ_GAP=0
ZTE_MESH_RUNDIR=$RUN
ZTE_MESH_ROOTFS=$ROOTFS
LIBEXEC=$T/libexec
ZTE_MESH_LIBEXEC=$LIBEXEC
# the clock every fixture is measured against: the hop-count grace window is seconds wide, so a suite that inherited
# the host's wall clock would judge or not judge depending on what minute someone ran it.
FIXED_NOW=1793686800
mkdir -p "$ROOTFS/root/lib" "$ROOTFS/root/bin" "$LIBEXEC"
export UBUS APK UCI IW IP LOGREAD DATE INIT_D RESTART_WAIT SSID_WAIT SSID_POLL METRICS_READS METRICS_READ_GAP \
	ZTE_MESH_RUNDIR ZTE_MESH_ROOTFS ZTE_MESH_LIBEXEC

pass=0
fail=0
out=$T/out

check_line() {
	# check_line LABEL EXPECTED -- the evidence line `LABEL=EXPECTED` must be there
	if grep -qx "$1=$2" "$out"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL case %s: expected %s=%s, got:\n' "$CASE" "$1" "$2"
		sed -e 's/^/    /' "$out"
	fi
}
check_rc() {
	if [ "$1" = "$2" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL case %s: exit expected=%s got=%s\n' "$CASE" "$1" "$2"
	fi
}

# --- stubs ----------------------------------------------------------------------------------------------------------
cat > "$BIN/ubus" <<EOF
#!/bin/sh
case "\$*" in
*luci.zte-mesh\ status*) cat '$FIX/status' ;;
*"service list"*)         cat '$FIX/svc' ;;
*) exit 1 ;;
esac
EOF
cat > "$BIN/uci" <<EOF
#!/bin/sh
case "\$*" in
*-q\ get*)   k=\${3##*.}; f='$FIX/uci/'"\$k"; [ -f "\$f" ] && cat "\$f" || case "\$*" in *mesh_role*) sed -n '1p' '$FIX/role' ;; esac ;;
*show\ wireless*) cat '$FIX/wireless' ;;
*) exit 1 ;;
esac
EOF
cat > "$BIN/apk" <<EOF
#!/bin/sh
case "\$1" in
add)
	if [ -f '$FIX/add-fails' ]; then echo 'ERROR: unable to select packages:'; exit 1; fi
	if [ -f '$FIX/uninstallable' ]; then echo 'ERROR: zte-mesh-0.1.0-r15: not installable'; exit 1; fi
	# naming the file it took is how the test catches the stem-glob confusion (zte-mesh swallowing the launcher apk)
	echo "ADDED $(basename "${2##*/}")"; exit 0 ;;
list) cat '$FIX/installed' ;;
*) exit 1 ;;
esac
EOF
cat > "$BIN/logread" <<EOF
#!/bin/sh
[ -f '$FIX/log' ] && cat '$FIX/log'
EOF
cat > "$BIN/date" <<EOF
#!/bin/sh
echo $FIXED_NOW
EOF
# The radio side of PLAN H-01: the script only greps these two, so one fixture per command carries the whole answer
# (`iw dev <if> info` and `iw dev <if> station dump` share theirs -- an ssid line, an Authorized line, no 4addr).
cat > "$BIN/iw" <<EOF
#!/bin/sh
[ -f '$FIX/iw' ] && cat '$FIX/iw'
EOF
cat > "$BIN/ip" <<EOF
#!/bin/sh
[ -f '$FIX/ip' ] && cat '$FIX/ip'
EOF
# The copied dispatcher asks its own machine through `uname -m`, and the script never asks it itself -- so this stub
# only answers for the launcher cases. Without it the suite would judge whatever CPU the test host happens to have.
cat > "$BIN/uname" <<EOF
#!/bin/sh
echo \${ZTE_MESH_TEST_MACHINE:-x86_64}
EOF
chmod +x "$BIN/uname"
mkdir -p "$FIX/uci"
chmod +x "$BIN/iw" "$BIN/ip"
chmod +x "$BIN/ubus" "$BIN/uci" "$BIN/apk" "$BIN/logread" "$BIN/date"
# The two service scripts an install has to activate; each records that it was asked to restart.
mkdir -p "$INIT_D"
for name in rpcd zte-mesh; do
	cat > "$INIT_D/$name" <<EOF
#!/bin/sh
# the read-back greps this file for the batch's own marker: a child that borrows its seed from the air defines
# live_ap_iface, and the r17 script on the lab machines has zero hits for it -- so the string here is the thing
# a packaged shell change leaves behind when nothing in the rootfs tarball moves. A12b's is mesh_compatible.
if [ "\$1" = restart ]; then
	echo '$name' >> '$FIX/restarted'
	# the applier's write, once per line this fixture says the restart made. Only zte-mesh: rpcd does not touch
	# /etc/config/wireless, and the log is the only witness of a restart that renamed the network.
	if [ '$name' = zte-mesh ] && [ -f '$FIX/writes' ]; then
		n=\$(cat '$FIX/writes'); i=0
		while [ "\$i" -lt "\$n" ]; do
			echo 'Mon Nov  3 09:00:00 UTC 2026 zte-mesh: applied; wireless committed and reloaded' >> '$FIX/log'
			i=\$((i + 1))
		done
	fi
	# the ring can roll over inside the window: the log left after the restart then holds fewer of those lines than
	# the log before it. That is not a quiet restart -- it is a truncated window, and the delta is not a number.
	if [ '$name' = zte-mesh ] && [ -f '$FIX/wrapped' ]; then cat '$FIX/wrapped' > '$FIX/log'; fi
fi
exit 0
EOF
	chmod +x "$INIT_D/$name"
done

# the applier the package installs next to the init script; the read-back wants the flag comparison this batch added
cat > "$LIBEXEC/wlan-sync" <<'EOF'
#!/bin/sh
set_flag() { :; }
EOF
chmod +x "$LIBEXEC/wlan-sync"

# the collector, same directory: the read-back wants the string that only the version with the pause switch carries
cat > "$LIBEXEC/sta-collect" <<'EOF'
#!/bin/sh
PAUSE=$DIR/collect-pause
EOF
chmod +x "$LIBEXEC/sta-collect"

# plan_rows SSID... -- /run/zte-mesh/wlan-plan is a key=value table, one row per BSS the applier was told to carry.
# The SSID smoke compares those rows against the wireless config, so the fixtures have to look like the real file.
plan_rows() {
	: > "$RUN/wlan-plan"
	printf 'rows=%d\n' "$#" >> "$RUN/wlan-plan"
	i=0
	for ssid in "$@"; do
		{
			echo "$i.radio=$i"
			echo "$i.slot=0"
			echo "$i.instance=DEV.WIFI.AP$i"
			echo "$i.ssid=$ssid"
			echo "$i.key=Ztemesh2631!"
			echo "$i.enabled=1"
			echo "$i.hidden=0"
			echo "$i.beacon_type=12"
		} >> "$RUN/wlan-plan"
		i=$((i + 1))
	done
}

# An instance's own `running` sits at four tabs and its name at three, exactly like `ubus call service list`.
instance() {
	printf '\t\t\t"%s": {\n' "$1"
	printf '\t\t\t\t"running": %s,\n' "$2"
	printf '\t\t\t\t"command": [\n\t\t\t\t\t"/usr/libexec/zte-mesh/%s"\n\t\t\t\t]\n' "$1"
	printf '\t\t\t}\n'
}
status_json() {
	# the same key order and the same nesting as openwrt/zte-mesh/files/luci.zte-mesh's status branch
	printf '{\n\t"installed": %s,\n\t"running": %s,\n\t"attached": %s,\n' "$1" "$2" "$3"
	printf '\t"role": "%s",\n\t"local": %s,\n\t"sync": null,\n\t"age": 1,\n' "$4" "$5"
	printf '\t"state": { "running": true, "schema": 1 }\n}\n'
}

# --- case 1: a child that is fine -----------------------------------------------------------------------------------
CASE=child-healthy
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf 'wireless.zte_backhaul_sta_5g=wifi-iface\nwireless.zte_backhaul_sta_5g.ssid=%sFRESHR10-5G%s\nwireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid=%sFRESHR10%s\n' "'" "'" "'" "'" > "$FIX/wireless"
printf 'zte-mesh-0.1.0-r15 noarch {/x} (GPL-2.0-only) [installed]\nzte-mesh-launcher-0.1.0-r2 aarch64_cortex-a53 {/y} (GPL-2.0-only) [installed]\nluci-app-zte-mesh-0.1.0-r8 noarch {/z} (GPL-2.0-only) [installed]\n' > "$FIX/installed"
printf 'epoch=1793000000\nresult=ok\nchanged=1\nbands=2g 5g\nmessage=\n' > "$RUN/wlan-sync.status"
plan_rows FRESHR10 FRESHR10-5G
printf 'Mon Nov  2 10:00:00 UTC 2026 zte-mesh: plan applied\n' > "$FIX/log"
printf 'Mon Nov  2 10:00:00 UTC 2026 ns-run[411]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=2 max_hop=2 epoch=1793000000\n' >> "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line restart not_needed
check_line applier_writes no-restart
check_line service_role slave
check_line instance_wlan-sync running
check_line instance_search absent
check_line sync_result ok
check_line sync_bands '2g 5g'
check_line profile_sections zte_backhaul_sta_5g
check_line plan_ssid_0 FRESHR10@default_radio0
check_line plan_ssid_1 'FRESHR10-5G@zte_backhaul_sta_5g'
check_line version_zte-mesh 0.1.0-r15
check_line version_luci-i18n-zte-mesh-zh-cn absent
# the machine still carries the package that used to hold ns-run and mesh-search; saying so is the whole line, and
# it is what makes `version_zte-mesh` below mean "this is the merged package" rather than "the solver kept both".
check_line legacy_launcher_package 0.1.0-r2
check_line log_error_lines 0
# the child's own hop count: the startup fixture writes -2 and nothing else in our code overwrites it on this role,
# so a 2 here can only be the vendor daemon's own update (B16).
check_line cur_hop 2
# no launcher in this fixture yet: an entry script that is simply not there is an environment fact -- the read-back
# reports which CPU it would have taken and judges nothing, so a healthy child still ends in PASS.
check_line launcher_dispatch unreadable
check_line launcher_arch not-installed

# profile_sections is a whole line, so compare it as text rather than through the value-splitting helper
if grep -q '^profile_sections=zte_backhaul_sta_5g$' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: profile_sections should name only our own section"; fi

# --- case 2: a controller that leaves its own radios alone -----------------------------------------------------------
CASE=controller-only
status_json true true true master false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'master\n' > "$FIX/role"
printf 'wireless.default_2g=wifi-iface\nwireless.default_5g=wifi-iface\n' > "$FIX/wireless"
rm -f "$RUN/wlan-sync.status"
plan_rows SHOULD-NOT-BE-APPLIED
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line service_local false
check_line instance_wlan-sync absent
check_line instance_search running
check_line profile_no_status_file absent
check_line profile_sections unset
if grep -q '^plan_ssid_' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a controller that applies nothing has no SSID to answer for"; else pass=$((pass + 1)); fi

# the same machine with an applier instance present is the signature bug this catches: the init stopped opening the
# instance but something still thinks it runs, or restart.sig now claims one the role never had
CASE=controller-only-stray-applier
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL instance_wlan-sync expected=absent got=running' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a stray applier must be the thing that fails"; fi

# --- case 3: a broadcasting controller (apply_local=1) ----------------------------------------------------------------
CASE=broadcasting-controller
status_json true true true master true > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'master\n' > "$FIX/role"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.zte_extra_2g_1=wifi-iface\nwireless.zte_extra_2g_1.ssid='FRESHR10+5G.bak'\nwireless.zte_backhaul_5g=wifi-iface\n" > "$FIX/wireless"
plan_rows FRESHR10 'FRESHR10+5G.bak'
printf 'epoch=1\nresult=ok\nchanged=0\nbands=2g\nmessage=wpad-basic has no Multi-AP support\n' > "$RUN/wlan-sync.status"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line service_local true
check_line instance_wlan-sync running
check_line sync_result ok
if grep -q '^profile_sections=zte_extra_2g_1 zte_backhaul_5g$' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: both of our sections must be named, in the order uci lists them"; fi
check_line plan_ssid_0 FRESHR10@default_radio0
check_line plan_ssid_1 'FRESHR10+5G.bak@zte_extra_2g_1'
if grep -q '^sync_message=wpad-basic has no Multi-AP support$' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: the applier's own message is evidence and must be printed"; fi

# --- case 4: the service is not running at all ------------------------------------------------------------------------
CASE=stalled
status_json true false false master false > "$FIX/status"
printf 'FAIL smoke_service expected=got\n' >/dev/null
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL service_running expected=true got=false' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a stalled service must fail the run"; fi

# --- case 5: applier said error ---------------------------------------------------------------------------------------
CASE=applier-error
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'epoch=1\nresult=error\nchanged=0\nbands=\nmessage=the plan could not be applied\n' > "$RUN/wlan-sync.status"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
check_line sync_result error

# --- case 6: install reporting -----------------------------------------------------------------------------------------
# The directory holds a zte-mesh-launcher-*.apk from the batch before the packages were merged. That file is not one
# of this batch's stems, and it must never be installed -- nor, with a plain `zte-mesh-*.apk` glob, mistaken for
# zte-mesh itself (docs/INSTALL.md §2: one directory, one batch).
CASE=install-mixed
printf 'not installable here\n' > "$FIX/uninstallable"
APK_DIR=$T/apks
mkdir -p "$APK_DIR"
: > "$APK_DIR/zte-mesh-0.1.0-r15.apk"
: > "$APK_DIR/zte-mesh-launcher-0.1.0-r2.apk"
: > "$APK_DIR/luci-app-zte-mesh-0.1.0-r8.apk"
sh "$SCRIPT" "$APK_DIR" > "$out" 2>&1
check_line install_zte-mesh uninstallable
check_line install_luci-app-zte-mesh uninstallable
check_line install_luci-i18n-zte-mesh-zh-cn skip
if grep -q '^install_zte-mesh-launcher=' "$out"; then
	fail=$((fail + 1)); echo "FAIL case $CASE: the leftover launcher apk was installed"
else
	pass=$((pass + 1))
fi
if grep -qx 'RESULT=PASS' "$out" || grep -qx 'RESULT=FAIL' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: install must still end with a verdict"; fi

# the two verdicts must not be confused: a file that is absent is a skip, a file the machine refuses is a failure.
# And each stem must pick its own file -- zte-mesh-[0-9]*.apk is what keeps the launcher file out of it.
CASE=install-clean
rm -f "$FIX/uninstallable" "$FIX/add-fails" "$FIX/restarted"
sh "$SCRIPT" "$APK_DIR" > "$out" 2>&1
check_line restart rpcd+zte-mesh
check_line install_zte-mesh zte-mesh-0.1.0-r15.apk
if [ "$(wc -l < "$FIX/restarted" 2>/dev/null | tr -d ' ')" = 2 ]; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: an install must restart both rpcd and zte-mesh"; fi
if grep -q 'zte-mesh-launcher-0.1.0-r2.apk' "$out"; then
	fail=$((fail + 1)); echo "FAIL case $CASE: the leftover launcher apk was installed"
else
	pass=$((pass + 1))
fi
check_line install_luci-app-zte-mesh luci-app-zte-mesh-0.1.0-r8.apk
check_line install_luci-i18n-zte-mesh-zh-cn skip

# --- case 7: the running rpcd is older than the package on disk -------------------------------------------------------
# `apk add` replaces the file; it does not reload the plugin. The status document then has no `local` key at all
# (luci.zte-mesh's status branch only got it with the role split), and a script that read the empty string as
# "not local" would report a clean controller.
CASE=stale-rpcd
{
	printf '{\n\t"installed": true,\n\t"running": true,\n\t"attached": true,\n\t"role": "master",\n'
	printf '\t"sync": null,\n\t"age": 1,\n\t"state": { "running": true }\n}\n'
} > "$FIX/status"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL service_local reported=absent hint=.*rpcd restart' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a missing local key must name the reload, not look like apply_local=0"; fi

# --- case 8: the log count means "unexplained" --------------------------------------------------------------------------
# The trace socket retry pair is logged by the dozen per hour on a healthy controller; a crash loop is not normal.
CASE=log-noise
status_json true true true slave false > "$FIX/status"
printf 'zte-mesh: ns-run: ZTE_CONTROLLER_TRACE unix_bind path=/var/tmp/zte-meshctl0._snd status=-1 errno=98\nzte-mesh: ns-run: ZTE_CONTROLLER_TRACE unix_bind path=/var/tmp/zte-meshctl0._snd status=0 errno=111\n' > "$FIX/log"
printf 'epoch=1\nresult=ok\nchanged=1\nbands=2g\nmessage=\n' > "$RUN/wlan-sync.status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
sh "$SCRIPT" > "$out" 2>&1
check_line log_error_lines 0
printf 'procd: Instance zte-mesh::search s in a crash loop 6 crashes, 300 seconds since last crash\n' >> "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_line log_error_lines 1

# --- case 9: the applier says ok but nothing carries the SSID ---------------------------------------------------------
# This is the shape the profile smoke exists for: wlan-sync reports result=ok after applying onto the radio's own
# section, so a check that only looks for `zte_*` sections sees `none` and calls the machine healthy. The plan names
# the SSIDs; the wireless config has to show at least one of them.
CASE=ssid-never-landed
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid=OldLocalName\n' > "$FIX/wireless"
plan_rows FRESHR10
printf 'epoch=1\nresult=ok\nchanged=1\nbands=2g\nmessage=\n' > "$RUN/wlan-sync.status"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL plan_ssid_0 expected=FRESHR10 got=absent' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: an SSID the applier claims but the config does not carry must fail"; fi
# the wait is what makes the FAIL mean "still wrong after the settle", so name the rows that never landed
check_line ssid_wait 0
check_line ssid_pending 0

# --- case 10: the installed copy is the one this batch wrote ---------------------------------------------------------
# The device copy cannot be hashed (apk strips the sections), so the script greps the two strings that only this
# batch's writer carries. A library without the per-BSS name still runs: the service sums an empty directory every
# second and falls back to the digest, so the page keeps the behaviour of the previous batch instead of breaking.
# That quiet fall-back is what these cases exist for.
packed_libs() {
	# packed_libs [stale] -- the two members the read-back greps; `stale` is the library from before write_bss()
	if [ "${1:-}" = stale ]; then
		printf 'light=%d metrics software ok\n' 1 > "$ROOTFS/root/lib/libzte_controller_trace.so"
	else
		printf 'light=%d metrics software .%%02x:%%02x:%%02x:%%02x:%%02x:%%02x.sta\n' 1 \
			> "$ROOTFS/root/lib/libzte_controller_trace.so"
	fi
	printf 'bss_detail\n' > "$ROOTFS/root/bin/db-engine-probe"
}
healthy_child() {
	status_json true true true slave false > "$FIX/status"
	{
		printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
		instance controller true
		instance attach true
		instance wlan-sync true
		printf '\t\t}\n\t}\n}\n'
	} > "$FIX/svc"
	printf 'slave\n' > "$FIX/role"
	printf 'wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid=FRESHR10\n' > "$FIX/wireless"
	plan_rows FRESHR10
	printf 'epoch=1\nresult=ok\nchanged=1\nbands=2g\nmessage=\n' > "$RUN/wlan-sync.status"
}
# launcher_tree ARCH... -- what the architecture-independent package lays down: the entry script twice (the two names
# the service runs) and one build per CPU under $LIBEXEC/arch/<cpu>/. The stub builds announce themselves, so an entry
# that execs its binary instead of answering the read-back's question is visible in the evidence instead of silent.
launcher_tree() {
	rm -rf "$LIBEXEC/arch"
	mkdir -p "$LIBEXEC/arch"
	for want in "$@"; do
		mkdir -p "$LIBEXEC/arch/$want"
		for name in ns-run mesh-search; do
			printf '#!/bin/sh\necho ran %s %s\n' "$name" "$want" > "$LIBEXEC/arch/$want/$name"
			chmod +x "$LIBEXEC/arch/$want/$name"
		done
	done
	for name in ns-run mesh-search; do
		cp "$DISPATCH" "$LIBEXEC/$name"
		chmod +x "$LIBEXEC/$name"
	done
}

CASE=packed-readback
healthy_child
packed_libs
mkdir -p "$RUN/peers"
printf 'light=0\n' > "$RUN/peers/02:00:00:00:19:e3.light"
printf 'hops=1\n' > "$RUN/peers/02:00:00:00:19:e3.metrics"
printf '{"nodes":[{"mac":"02:00:00:00:19:e3","zte":true,"online":true,"light":-1,"light_age":-1,"stations":-1,"bss":0,"hops":1,"metrics_age":30,"bss_detail":[]}]}\n' > "$RUN/state.json"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line trace_per_bss_name yes
check_line probe_bss_detail yes
check_line init_seed_from_air yes
check_line sync_flag_by_meaning yes
check_line collect_pause_in_collector yes
check_line peers_sta 0
check_line peers_software 0
# the lab link's only metrics digest is hops=1: no station count arrived at all, so an empty per-BSS count is not
# evidence against the decoder and must not be graded as one
check_line peer_reports_stations no
# one snapshot (METRICS_READS=1 for the whole suite) and no number in it, from either record
check_line peer_reads 1
check_line peer_input no
# a controller does not run the collector, so the table simply is not there -- that is the role's shape, not a fault
check_line ap_sta_rows absent
check_line ap_sta_age absent
check_line state_bss_detail present
# the counts beside that condition: one peer this machine heard from, nothing it ever split, and one child row the
# controller still calls online. These are the H-12/H-13 numbers with the companion line that makes them mean
# something -- `peers_seen=0` would make a zero software count say nothing about the Device Inventory TLV.
check_line peers_seen 1
check_line sta_file_sum 0
check_line peer_software_bytes 0
check_line bss_detail_rows 0
check_line nodes_rows 1
check_line nodes_online 1

# the same machine with the files a real child produces: two BSS rows and a version string
CASE=packed-readback-full
printf '3\n' > "$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e4.sta"
printf '1\n' > "$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e5.sta"
printf 'V1.0.0.2B2.8000\n' > "$RUN/peers/02:00:00:00:19:e3.software"
printf 'stations=4\nhops=1\n' > "$RUN/peers/02:00:00:00:19:e3.metrics"
# the collector's table on the child side of the same link: what the stand-in is asked to answer from
printf 'epoch=%s\n02:00:00:00:19:e4 3\n02:00:00:00:19:e5 1\n' "$((FIXED_NOW - 30))" > "$RUN/ap-sta"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line peers_sta 2
check_line peers_software 1
check_line peer_reports_stations yes
check_line peer_reads 1
check_line peer_input yes
check_line ap_sta_rows 2
check_line ap_sta_age 30

# the batch-18 reading, spelled out: per-BSS records present, digest with no station count in it. That pair is by
# design (the stand-in answers 0x94 and never writes the aggregate TLV), and `peer_input` is the line that says the
# controller did get numbers -- `peer_reports_stations` stays the narrower snapshot it always was.
CASE=peer-input-from-split-only
printf 'hops=1\n' > "$RUN/peers/02:00:00:00:19:e3.metrics"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line peers_sta 2
check_line peer_reports_stations no
check_line peer_input yes

# an empty table is a measured nothing, not a missing collector: the row count says zero and the age still counts
CASE=ap-sta-empty
printf 'epoch=%s\n' "$((FIXED_NOW - 42))" > "$RUN/ap-sta"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line ap_sta_rows 0
check_line ap_sta_age 42

# a collector stopped by the pause flag leaves its last table sitting there and stops stamping it: the rows stay, the
# age grows, and that is the reading H-15 ⑤ wants. Nothing is judged -- a stale table is the responder's business.
CASE=ap-sta-paused
printf 'epoch=%s\n02:00:00:00:19:e4 3\n' "$((FIXED_NOW - 120))" > "$RUN/ap-sta"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line ap_sta_rows 1
check_line ap_sta_age 120

# a table with no epoch line has no age to report. The row count still stands: half the reading is better than
# dropping the line, and `unreadable` says which half is missing.
CASE=ap-sta-no-epoch
printf '02:00:00:00:19:e4 3\n02:00:00:00:19:e5 1\n' > "$RUN/ap-sta"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line ap_sta_rows 2
check_line ap_sta_age unreadable

# --- the second snapshot: one read is one instant (the batch-18 lesson) -----------------------------------------------
# A frame with no station count rewrites the metrics line without it, so a single snapshot can answer `no` while the
# per-BSS records say otherwise. The script re-reads only while a pass shows nothing anywhere; the proof that it
# re-reads is a fixture that changes inside the gap. `sleep` is stubbed for this call alone -- a real five seconds
# would make the case depend on the host, and METRICS_READ_GAP=0 would make the loop finish before anything lands.
CASE=peer-resample
printf 'epoch=%s\n02:00:00:00:19:e4 3\n' "$((FIXED_NOW - 42))" > "$RUN/ap-sta"
rm -f "$RUN/peers"/*.sta "$RUN/peers"/*.software
printf 'hops=1\n' > "$RUN/peers/02:00:00:00:19:e3.metrics"
cat > "$BIN/sleep" <<EOF
#!/bin/sh
# the gap is when the frame arrives: one per-BSS record, written by the first sleep this run takes
[ -f '$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e4.sta' ] || printf '3\n' > '$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e4.sta'
exit 0
EOF
chmod +x "$BIN/sleep"
PATH="$BIN:$PATH" METRICS_READS=3 METRICS_READ_GAP=5 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line peer_reads 2
check_line peers_sta 1
check_line peer_reports_stations no
check_line peer_input yes
check_line ap_sta_rows 1
check_line ap_sta_age 42
rm -f "$BIN/sleep" "$RUN/ap-sta"
printf '3\n' > "$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e4.sta"
printf '1\n' > "$RUN/peers/02:00:00:00:19:e3.02:00:00:00:19:e5.sta"
printf 'V1.0.0.2B2.8000\n' > "$RUN/peers/02:00:00:00:19:e3.software"
printf 'stations=4\nhops=1\n' > "$RUN/peers/02:00:00:00:19:e3.metrics"

# the document those same frames leave behind: write_state sums the .sta files into the node row, so `sta_file_sum`
# and `bss_detail_rows` are two readings of one witness. Neither is judged -- an empty split is H-13's answer, not a
# fault -- but the script has to count them against the shape the daemon really writes, or it reads zero forever.
CASE=peer-evidence-split-rows
printf '{"nodes":[{"mac":"02:00:00:00:19:e3","zte":true,"online":true,"light":-1,"light_age":-1,"stations":4,"bss":2,"hops":1,"metrics_age":30,"bss_detail":[{"bssid":"02:00:00:00:19:e4","stations":3,"age":12},{"bssid":"02:00:00:00:19:e5","stations":1,"age":12}]}]}' > "$RUN/state.json"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line sta_file_sum 4
check_line peer_software_bytes 16
check_line bss_detail_rows 2
check_line nodes_rows 1
check_line nodes_online 1

CASE=packed-stale
packed_libs stale
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL trace_per_bss_name expected=yes got=no' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a library that never writes the per-BSS file must fail the run"; fi

# --- case 11: no rootfs on this machine is not a stale rootfs --------------------------------------------------------
# This harness has no private root at all; turning that environment fact into a batch verdict would fail every run.
CASE=packed-absent
rm -f "$ROOTFS/root/lib/libzte_controller_trace.so" "$ROOTFS/root/bin/db-engine-probe"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line trace_per_bss_name unreadable
check_line probe_bss_detail unreadable

# --- the batch that only moves shell files: the package itself is the witness ------------------------------------------
# r18 changes `zte-mesh.init` and `wlan-sync` and nothing in the rootfs, so `packed_copy` stays `yes` on a machine
# still running the old scripts. A batch whose fix is invisible to the read-back is a batch that can be reported as
# installed while it is not installed.
CASE=scripts-stale
packed_libs
sed -i 's/live_ap_iface/borrowed_seed/' "$INIT_D/zte-mesh"
# the A12b reader, spelled the way an r18 init spells it: still reading the retired key
sed -i 's/mesh_compatible/mesh_v2/' "$INIT_D/zte-mesh"
printf '#!/bin/sh\n' > "$LIBEXEC/wlan-sync"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
for label in init_seed_from_air sync_flag_by_meaning init_reads_mesh_compatible; do
	if grep -q "^FAIL $label expected=yes got=no" "$out"; then pass=$((pass + 1)); else
		fail=$((fail + 1)); echo "FAIL case $CASE: an $label that is not in the package must fail the run"; fi
done
# the other two markers answer yes: the old machine is not stale in that direction, and the failure must be
# attributable to the one thing that is missing
check_line trace_per_bss_name yes
check_line probe_bss_detail yes
sed -i 's/borrowed_seed/live_ap_iface/' "$INIT_D/zte-mesh"
sed -i 's/mesh_v2/mesh_compatible/' "$INIT_D/zte-mesh"

# and a machine where the applier file simply is not there is not a stale one either
CASE=scripts-absent
packed_libs
printf '#!/bin/sh\nset_flag() { :; }\n' > "$LIBEXEC/wlan-sync"
rm -f "$LIBEXEC/wlan-sync"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line init_seed_from_air yes
check_line sync_flag_by_meaning unreadable
if grep -q '^FAIL ' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: an absent applier file is an environment fact"; else pass=$((pass + 1)); fi
printf '#!/bin/sh\nset_flag() { :; }\n' > "$LIBEXEC/wlan-sync"

# and the same judgement one file over: a machine whose collector cannot be paused is a machine where H-15 ⑤ still
# needs a procd edit -- and the edit is the one that restarts the daemon.
CASE=scripts-stale-collector
packed_libs
printf '#!/bin/sh\n' > "$LIBEXEC/sta-collect"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL collect_pause_in_collector expected=yes got=no' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a collector without the pause switch must fail the run"; fi
check_line init_seed_from_air yes
check_line sync_flag_by_meaning yes
printf '#!/bin/sh\nPAUSE=$DIR/collect-pause\n' > "$LIBEXEC/sta-collect"

# --- the launcher no longer picks a board at install time -------------------------------------------------------------
# The apk is architecture-independent now, so `apk add` accepts it on any machine and the gate that used to refuse the
# wrong board is gone. The read-back is where that answer lives: the entry script has to be installed, under both of
# the names the service runs, and it has to resolve to a build for the CPU it is standing on.
CASE=launcher-carried
healthy_child
packed_libs
rm -rf "$RUN/peers" "$RUN/ap-sta"
launcher_tree aarch64 armv7 x86_64
PATH="$BIN:$PATH" ZTE_MESH_TEST_MACHINE=x86_64 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line launcher_dispatch yes
check_line launcher_second_name yes
check_line launcher_arch_present yes
# the path is the answer the service would get too, which is why the read-back asks the entry instead of mapping the
# CPU here: this line proves the installed dispatch, not a copy of its table
check_line launcher_arch "$LIBEXEC/arch/x86_64/ns-run"

# a machine still holding the binary that the previous workflow built by hand is not a stale batch: no marker, so the
# read-back reports and judges nothing -- someone's own /usr/libexec must not fail the run
CASE=launcher-entry-without-marker
printf '#!/bin/sh\necho ran the binary the previous workflow built by hand\n' > "$LIBEXEC/ns-run"
chmod +x "$LIBEXEC/ns-run"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line launcher_dispatch no
check_line launcher_arch not-installed
if grep -q '^FAIL launcher' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: an entry script without the dispatcher is an environment fact"; else pass=$((pass + 1)); fi

# and the machine the package does not carry: the entry is installed, it answers, and the answer is 127 plus the list
# of architectures this apk really holds. That is the question the deleted gate used to ask, in words.
CASE=launcher-machine-not-carried
launcher_tree aarch64 armv7 x86_64
PATH="$BIN:$PATH" ZTE_MESH_TEST_MACHINE=mips64 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL launcher_arch_present expected=yes got=no' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a machine with no build for it must fail the run"; fi
check_line launcher_arch 'zte-mesh ns-run: no build for this machine (uname -m says mips64). The launcher on this box carries: aarch64 armv7 x86_64'
# the failure is the missing build, not a missing package
check_line launcher_dispatch yes
check_line launcher_second_name yes

# back to the way the rest of the suite found it: no entry script, so no later case turns on the CPU of the host
rm -f "$LIBEXEC/ns-run" "$LIBEXEC/mesh-search"
rm -rf "$LIBEXEC/arch"

# --- case 12: the profile lands a few seconds after the restart -------------------------------------------------------
# Measured on ax3600 (2026-11-03): after a restart the plan has no rows at t=0, its rows are there at t=2s while
# /etc/config/wireless still carries the previous profile, and the applier lands the new one at t=4s. Grading once
# right after the restart -- which is what INSTALL section 3 tells a reader to do after an install -- failed a
# healthy child. The stub answers with the old name for the first two `show wireless` calls (the profile smoke's
# own read, then the first settle pass) and with the landed name after that, so the wait is what turns it green.
CASE=ssid-late-landed
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf 'wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid=OldLocalName\n' > "$FIX/wireless"
printf 'wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid=FRESHR10\n' > "$FIX/wireless-landed"
plan_rows FRESHR10
printf 'epoch=1\nresult=ok\nchanged=1\nbands=2g\nmessage=\n' > "$RUN/wlan-sync.status"
rm -f "$FIX/count"
cat > "$BIN/uci" <<EOF
#!/bin/sh
case "\$*" in
*-q\ get*)   k=\${3##*.}; f='$FIX/uci/'"\$k"; [ -f "\$f" ] && cat "\$f" || case "\$*" in *mesh_role*) sed -n '1p' '$FIX/role' ;; esac ;;
*show\ wireless*)
	n=0
	[ -f '$FIX/count' ] && n=\$(cat '$FIX/count')
	n=\$((n + 1)); printf '%s\n' "\$n" > '$FIX/count'
	if [ "\$n" -ge 3 ]; then cat '$FIX/wireless-landed'; else cat '$FIX/wireless'; fi ;;
*) exit 1 ;;
esac
EOF
chmod +x "$BIN/uci"
SSID_WAIT=6 SSID_POLL=2 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line ssid_wait 2
check_line plan_ssid_0 FRESHR10@default_radio0
if grep -q '^ssid_pending' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a settled profile must not report pending rows"; else pass=$((pass + 1)); fi

# back to the stub every other case uses
cat > "$BIN/uci" <<EOF
#!/bin/sh
case "\$*" in
*-q\ get*)   k=\${3##*.}; f='$FIX/uci/'"\$k"; [ -f "\$f" ] && cat "\$f" || case "\$*" in *mesh_role*) sed -n '1p' '$FIX/role' ;; esac ;;
*show\ wireless*) cat '$FIX/wireless' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$BIN/uci"

# --- case 13: a restart of a converged child writes nothing (B8b, PLAN H-14) ------------------------------------------
# The child's seed used to be the profile declared on this router, so every restart renamed the air and the
# controller's M2 renamed it back. `/etc/config/wireless` ends up identical either way -- the number of applier
# writes inside the restart window is the only thing that tells the two apart.
CASE=child-restart-quiet
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\n" > "$FIX/wireless"
plan_rows FRESHR10 FRESHR10-5G
printf 'epoch=1\nresult=ok\nchanged=0\nbands=\nmessage=\n' > "$RUN/wlan-sync.status"
printf 'Mon Nov  3 08:00:00 UTC 2026 zte-mesh: plan applied nothing new\n' > "$FIX/log"
rm -f "$FIX/writes"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line restart now
check_line air_matched_plan yes
check_line applier_writes 0

# --- case 14: the same machine with the old seed writes twice ---------------------------------------------------------
# This is r17's shape: the air already carried the controller's pair, the restart still renamed it and renamed it
# back. It must not pass on the strength of the file looking fine afterwards.
CASE=child-restart-renamed
printf '2\n' > "$FIX/writes"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL applier_writes expected=0 got=2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a restart that renamed the network must fail on the write count"; fi
rm -f "$FIX/writes"

# --- case 15: a child that still had to converge is evidence, not a fault ---------------------------------------------
# Fresh install, first run: the air carries the router's own network, so the applier has work to do and one write is
# correct. Judging that would call a healthy install broken -- the same class of mistake as grading the SSID too early.
CASE=child-restart-converging
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='OldLocalName'\n" > "$FIX/wireless"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\n" > "$FIX/wireless-landed"
plan_rows FRESHR10
printf '2\n' > "$FIX/writes"
rm -f "$FIX/count"
cat > "$BIN/uci" <<EOF
#!/bin/sh
case "\$*" in
*-q\ get*)   k=\${3##*.}; f='$FIX/uci/'"\$k"; [ -f "\$f" ] && cat "\$f" || case "\$*" in *mesh_role*) sed -n '1p' '$FIX/role' ;; esac ;;
*show\ wireless*)
	n=0
	[ -f '$FIX/count' ] && n=\$(cat '$FIX/count')
	n=\$((n + 1)); printf '%s\n' "\$n" > '$FIX/count'
	# the first read is the one taken before the restart; from the settle pass on, the plan has landed
	if [ "\$n" -ge 2 ]; then cat '$FIX/wireless-landed'; else cat '$FIX/wireless'; fi ;;
*) exit 1 ;;
esac
EOF
chmod +x "$BIN/uci"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line air_matched_plan rows-unset:0
check_line applier_writes 2
if grep -q 'FAIL applier_writes' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: nothing to judge while the profile is still landing"; else pass=$((pass + 1)); fi
rm -f "$FIX/writes" "$FIX/wireless-landed"

# back to the stub every other case uses
cat > "$BIN/uci" <<EOF
#!/bin/sh
case "\$*" in
*-q\ get*)   k=\${3##*.}; f='$FIX/uci/'"\$k"; [ -f "\$f" ] && cat "\$f" || case "\$*" in *mesh_role*) sed -n '1p' '$FIX/role' ;; esac ;;
*show\ wireless*) cat '$FIX/wireless' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$BIN/uci"

# a controller that touches no radio has no applier to count -- and asking for the restart must not invent one
CASE=controller-restart-no-applier
status_json true true true master false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'master\n' > "$FIX/role"
rm -f "$RUN/wlan-sync.status"
printf 'Mon Nov  3 08:00:00 UTC 2026 zte-mesh: plan applied nothing new\n' > "$FIX/log"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line applier_writes no-applier
if grep -q '^air_matched_plan' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: the write window is not this role's business"; else pass=$((pass + 1)); fi

# --- case 16: no log to count -- zero lines is not a quiet restart ----------------------------------------------------
# `grep -c` on an unreadable log answers 0, and 0 - 0 is exactly the shape of the verdict we want. The only honest
# answer on a machine that exposes no log is "I did not look".
CASE=child-restart-no-logread
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\n" > "$FIX/wireless"
plan_rows FRESHR10 FRESHR10-5G
printf 'epoch=1\nresult=ok\nchanged=0\nbands=\nmessage=\n' > "$RUN/wlan-sync.status"
rm -f "$FIX/log" "$FIX/writes" "$FIX/wrapped"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line applier_writes no-logread
if grep -q '^FAIL applier_writes' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a log that cannot be read is not a fault"; else pass=$((pass + 1)); fi

# --- case 17: the log ring rolled over inside the window --------------------------------------------------------------
# Three committed lines before the restart, none after: the window was truncated, so the delta would be -3.
CASE=child-restart-wrapped
printf 'Mon Nov  3 08:00:00 UTC 2026 zte-mesh: applied; wireless committed and reloaded\n' > "$FIX/log"
printf 'Mon Nov  3 08:00:00 UTC 2026 zte-mesh: applied; wireless committed and reloaded\n' >> "$FIX/log"
printf 'Mon Nov  3 08:00:00 UTC 2026 zte-mesh: applied; wireless committed and reloaded\n' >> "$FIX/log"
printf 'Mon Nov  3 08:30:00 UTC 2026 zte-mesh: plan applied nothing new\n' > "$FIX/wrapped"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line applier_writes wrapped
if grep -q '^FAIL applier_writes' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a truncated window is not a count"; else pass=$((pass + 1)); fi
rm -f "$FIX/wrapped"

# --- case 18: the child's hop count is still the `not attached` the fixture wrote (B16) --------------------------------
# -2 is what our own startup path writes for a child; the daemon is supposed to replace it. A machine that has been
# up and still reads -2 has an agent that never learned its hop count -- that is the fault this line exists to catch.
CASE=cur-hop-not-attached
printf 'Mon Nov  3 09:00:00 UTC 2026 ns-run[77]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
check_line cur_hop_age 8800
if grep -q 'FAIL cur_hop expected=updated got=-2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a child that never updated CurHop must be named"; fi

# --- case 19: no cfg line in the ring yet is not a verdict ------------------------------------------------------------
CASE=cur-hop-no-line
printf 'Mon Nov  3 09:00:00 UTC 2026 zte-mesh: plan applied nothing new\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line cur_hop no-cfg-line
if grep -q '^FAIL cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: nothing observed is not a failure"; else pass=$((pass + 1)); fi

# -98 is the supervisor's own `could not read the column`, which is not a hop count either
CASE=cur-hop-unreadable
printf 'Mon Nov  3 09:00:00 UTC 2026 ns-run[77]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=-98 max_hop=-98 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line cur_hop -98
if grep -q '^FAIL cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: an unreadable column is the service's problem, not this batch's"; else pass=$((pass + 1)); fi

# a controller writes this column itself (multiap-cfg.c), so its 2 says nothing about the agent: evidence only
CASE=cur-hop-controller-evidence
status_json true true true master false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'master\n' > "$FIX/role"
rm -f "$RUN/wlan-sync.status"
plan_rows SHOULD-NOT-BE-APPLIED
printf 'Mon Nov  3 09:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=2 set_role_enable=0 en_master_only=1 cur_hop=-2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line cur_hop -2
if grep -q '^FAIL cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: this role is not the agent path"; else pass=$((pass + 1)); fi

# and a restart is not the window for this number: the daemon writes the column seconds to minutes after a fresh
# start, and the new supervisor's own baseline snapshot can land after we look -- the C10 lesson, one column over.
CASE=cur-hop-after-restart
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\n" > "$FIX/wireless"
plan_rows FRESHR10 FRESHR10-5G
printf 'epoch=1\nresult=ok\nchanged=0\nbands=\nmessage=\n' > "$RUN/wlan-sync.status"
printf 'Mon Nov  3 09:00:00 UTC 2026 ns-run[77]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
RESTART_NOW=1 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line cur_hop -2
if grep -q '^FAIL cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: the run that just restarted the supervisor cannot grade its hop count"; else pass=$((pass + 1)); fi

# --- case 20: a witness that is fifty seconds old is a daemon that has not been told yet --------------------------------
# This is what the r18 install round on ax3600 actually did: the restart put `cur_hop=-2` in the ring at 13:26:51 and
# `cur_hop=2` at 13:27:18. The first read-only run landed inside that window and answered RESULT=FAIL about a healthy
# child -- the same "问得太早" mistake the SSID smoke had to fix, one column over.
CASE=cur-hop-young
printf 'Mon Nov  3 13:26:51 UTC 2026 ns-run[10961]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2 max_hop=2 epoch=%s\n' "$((FIXED_NOW - 50))" > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line cur_hop -2
check_line cur_hop_age 50
if grep -q '^FAIL cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a settling child must be reported, not judged"; else pass=$((pass + 1)); fi

# and the window is the thing deciding: with no grace at all the same line is a failure again. Without this the young
# case would pass for any reason -- a missing epoch field, a comparison that never fires -- and look like a fix.
CASE=cur-hop-young-no-grace
CUR_HOP_GRACE=0 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q 'FAIL cur_hop expected=updated got=-2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: the grace window must be what excuses the young witness"; fi

# a line without the stamp has no age to compute, so nothing excuses it: the judgement stands rather than silently
# skipping the check on a build that stopped writing `epoch=`.
CASE=cur-hop-no-epoch
printf 'Mon Nov  3 13:26:51 UTC 2026 ns-run[10961]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2 max_hop=2\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line RESULT FAIL
if grep -q '^cur_hop_age' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: no stamp means no age to report"; else pass=$((pass + 1)); fi
if grep -q 'FAIL cur_hop expected=updated got=-2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: an unstamped -2 on a child must still be named"; fi

# --- case 21: the lab CurHop override reached the controller's own row (B16b, PLAN H-14 ⑦ 后半) -----------------------
# The switch is one line in /run/zte-mesh/db-profile, a file no package writes. Before a child's number can be read
# as an answer to "TLV or MaxHop", the controller has to be shown to carry that number itself -- a field the daemon
# rejected would leave the child at 2 and look like a conclusion.
status_json true true true master false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance search true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'master\n' > "$FIX/role"
rm -f "$RUN/wlan-sync.status"
plan_rows SHOULD-NOT-BE-APPLIED
printf 'MultiAPCfg 0 CurHop 5\n' > "$RUN/db-profile"
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=2 set_role_enable=0 en_master_only=1 cur_hop=5 max_hop=2 epoch=1793678000\n' > "$FIX/log"
CASE=cur-hop-override-live
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line RESULT PASS
check_line db_profile_cur_hop 5
check_line master_cur_hop 5
# the two columns of the same row, side by side: this is the pair a child's own reading is compared against
check_line max_hop 2

# a profile the daemon did not take is the failure this line exists for -- the child would read 2 either way
CASE=cur-hop-override-ignored
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=2 set_role_enable=0 en_master_only=1 cur_hop=2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
if grep -q 'FAIL master_cur_hop expected=5 got=2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: an override that never reached the row must be named"; fi

# and the window that excuses it is the same one the child's number uses: the supervisor's baseline snapshot is
# written seconds after a restart, before the daemon has had the row read back to it
CASE=cur-hop-override-young
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=2 set_role_enable=0 en_master_only=1 cur_hop=2 max_hop=2 epoch=%s\n' "$((FIXED_NOW - 50))" > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line master_cur_hop 2
if grep -q '^FAIL master_cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a witness this young says nothing about the row"; else pass=$((pass + 1)); fi
CASE=cur-hop-override-young-no-grace
CUR_HOP_GRACE=0 sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
if grep -q 'FAIL master_cur_hop expected=5 got=2' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: the grace window must be what excuses that same line"; fi

# a machine with no profile is the normal machine: the override line is reported as absent and nothing is judged
CASE=cur-hop-no-override
rm -f "$RUN/db-profile"
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=2 set_role_enable=0 en_master_only=1 cur_hop=2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line db_profile_cur_hop absent
if grep -q '^master_cur_hop' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a machine that set no override gets no verdict about one"; else pass=$((pass + 1)); fi

# a profile that carries other tables and other fields is still no override
CASE=cur-hop-override-other-field
printf 'MultiAPMaster 0 EnBtmKickoff 1\nP1905ComCfg 0 Enable 1\n' > "$RUN/db-profile"
sh "$SCRIPT" > "$out" 2>&1
check_line db_profile_cur_hop absent
rm -f "$RUN/db-profile"

# no witness line in the ring, an unreadable column, a profile asking for a number: three shapes that report and judge
# nothing, none of them a fault of this batch
CASE=cur-hop-override-no-line
printf 'MultiAPCfg 0 CurHop 5\n' > "$RUN/db-profile"
printf 'Mon Nov  3 14:00:00 UTC 2026 zte-mesh: plan applied nothing new\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line master_cur_hop no-cfg-line
CASE=cur-hop-override-unreadable
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[88]: ZTE_MESH_SERVICE cfg mode=-98 set_role_enable=-98 en_master_only=-98 cur_hop=-98 max_hop=-98 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line master_cur_hop not-read

# on a child the same file is not ours to verify: its startup path puts -2 back over the profile, and the number that
# appears afterwards is the daemon's answer to the air -- which is `smoke_cur_hop`, a different judgement.
CASE=cur-hop-override-on-child
status_json true true true slave false > "$FIX/status"
{
	printf '{\n\t"zte-mesh": {\n\t\t"instances": {\n'
	instance controller true
	instance attach true
	instance wlan-sync true
	printf '\t\t}\n\t}\n}\n'
} > "$FIX/svc"
printf 'slave\n' > "$FIX/role"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\n" > "$FIX/wireless"
plan_rows FRESHR10 FRESHR10-5G
printf 'epoch=1\nresult=ok\nchanged=0\nbands=\nmessage=\n' > "$RUN/wlan-sync.status"
printf 'Mon Nov  3 14:00:00 UTC 2026 ns-run[77]: ZTE_MESH_SERVICE cfg mode=1 set_role_enable=0 en_master_only=0 cur_hop=2 max_hop=2 epoch=1793678000\n' > "$FIX/log"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line db_profile_cur_hop 5
check_line master_cur_hop not-master
check_line cur_hop 2
rm -f "$RUN/db-profile"

# --- the MESH compatibility switch: the page key and the daemon key answer opposite questions (A12b) -----------------
# `mesh_compatible=1` (the page's MESH 兼容 on, accept any child router) has to reach /run/zte-mesh/config as
# `mesh.v2=0`. Two shapes are failures and one is nothing at all: a writer that does not invert, a reader that still
# honours the retired key, and a machine whose daemon is not running (then there is no line to compare).
CASE=mesh-compat-inverted-ok
mkdir -p "$FIX/uci"
printf '1\n' > "$FIX/uci/mesh_compatible"
printf 'mesh.role=slave\nmesh.v2=0\n' > "$RUN/config"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line mesh_compat_uci 1
check_line mesh_v2_line 0
check_line mesh_compat_inverted 0
check_line RESULT PASS

# the un-inverted writer: the page asks for compatibility, the daemon is told strict admission
CASE=mesh-compat-not-inverted
printf 'mesh.role=slave\nmesh.v2=1\n' > "$RUN/config"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
if grep -q 'FAIL mesh_compat_inverted expected=0 got=1' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: the page value must reach the service file inverted"; fi

# and the other direction: 0 on the page is strict, so the line has to read 1
CASE=mesh-compat-off
printf '0\n' > "$FIX/uci/mesh_compatible"
printf 'mesh.role=slave\nmesh.v2=1\n' > "$RUN/config"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line mesh_compat_inverted 1
check_line RESULT PASS

# the retired key alone: an r18 conffile still holding mesh_v2, no page key. Nobody reads it, so the service file
# carries no mesh.v2 line -- and a line there can only have come from a reader that still honours the retired name.
CASE=mesh-compat-retired
rm -f "$FIX/uci/mesh_compatible"
printf '1\n' > "$FIX/uci/mesh_v2"
printf 'mesh.role=slave\nmesh.v2=1\n' > "$RUN/config"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
check_line mesh_compat_uci unset
check_line mesh_v2_legacy_uci 1
if grep -q 'FAIL mesh_compat_inverted expected=absent got=1' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: the retired key must not drive the line"; fi

CASE=mesh-compat-no-config
rm -f "$RUN/config" "$FIX/uci/mesh_v2" "$FIX/uci/mesh_compatible"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line mesh_v2_line no-config

# --- the wireless backhaul as the child holds it (PLAN H-01) --------------------------------------------------------
# The three uci lines this package writes are judged on a slave asked for multi_ap -- without `wds` netifd refuses the
# bridge (BRIDGE_NOT_ALLOWED) and 4addr never opens, and without `multi_ap 1` the supplicant will not even associate
# to a backhaul BSS. The radio's own answers (iface up, an ssid, a station, the two 4addr greps) are reported.
backhaul_child() {
	printf 'multi_ap\n' > "$FIX/uci/backhaul"
	printf 'sta\n' > "$FIX/uci/mode"
	printf '1\n' > "$FIX/uci/multi_ap"
	printf '1\n' > "$FIX/uci/wds"
	printf 'br-lan\n' > "$FIX/uci/network"
	printf 'radio1\n' > "$FIX/uci/device"
	printf '0\n' > "$FIX/uci/disabled"
	printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\nwireless.zte_backhaul_sta_5g=wifi-iface\n" > "$FIX/wireless"
	printf 'Interface zte_backhaul_sta_5g\n\ttype station\n\tssid ZTE-BH-5G\n\tfrequency 5745\nStation 02:00:00:00:19:e4 (on zte_backhaul_sta_5g)\n\tauthorized: yes\n\tsignal: -42 dBm\n' > "$FIX/iw"
	printf '9: zte_backhaul_sta_5g: <BROADCAST,MULTICAST,UP> mtu 1500\n' > "$FIX/ip"
}

CASE=backhaul-child-ok
backhaul_child
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line backhaul_asked multi_ap
check_line backhaul_section_5g zte_backhaul_sta_5g
check_line backhaul_mode_5g sta
check_line backhaul_multi_ap_5g 1
check_line backhaul_wds_5g 1
check_line backhaul_network_5g br-lan
check_line backhaul_iface_5g up
check_line backhaul_assoc_5g yes
check_line backhaul_station_5g authorized
# no 4addr in either command's output here: that is a reading about these two tools, not a broken applier, so the
# line is reported and the run still passes. Which build names the flag is the on-machine window's answer.
check_line backhaul_4addr_iw_5g no
check_line backhaul_4addr_ip_5g no
check_line RESULT PASS

# the known trap: the section exists and netifd will not bridge it
CASE=backhaul-child-no-wds
printf '0\n' > "$FIX/uci/wds"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 1
if grep -q 'FAIL backhaul_wds_5g expected=1 got=0' "$out"; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL case $CASE: a backhaul client without 4-address must fail the run"; fi

# asked for a mode the writer never produces: the section is the plan's, not this switch's, so nothing is judged
CASE=backhaul-not-asked
backhaul_child
printf 'sta\n' > "$FIX/uci/mode"
printf '0\n' > "$FIX/uci/multi_ap"
printf 'off\n' > "$FIX/uci/backhaul"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line backhaul_asked off
if grep -q '^FAIL backhaul' "$out"; then fail=$((fail + 1)); echo "FAIL case $CASE: a machine not asked for multi_ap gets no verdict about it"; else pass=$((pass + 1)); fi

# a child that has never been given a backhaul row: the section does not exist, and that is the controller's story
CASE=backhaul-no-section
printf 'off\n' > "$FIX/uci/backhaul"
printf "wireless.default_radio0=wifi-iface\nwireless.default_radio0.ssid='FRESHR10'\nwireless.default_radio1=wifi-iface\nwireless.default_radio1.ssid='FRESHR10-5G'\n" > "$FIX/wireless"
sh "$SCRIPT" > "$out" 2>&1
check_rc $? 0
check_line backhaul_asked off
check_line backhaul_section none
check_line RESULT PASS

printf 'verify-install test: %d passed, %d failed\\n' "$pass" "$fail"
[ "$fail" = 0 ]
