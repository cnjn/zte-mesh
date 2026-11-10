#!/bin/sh
# Host test of write_config() in openwrt/zte-mesh/files/zte-mesh.init without OpenWrt: the UCI shell functions are
# replaced by a tiny reader of real UCI syntax. Run with the shell OpenWrt uses:  busybox ash tests/test_init.sh [INIT]
# DUMP_DIR=dir keeps every produced service config there (dev-config-check.py feeds them to the real service).
# Every case writes a UCI file, runs write_config and compares the service config it produced (or the refusal).
INIT=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/zte-mesh.init}
T=$(mktemp -d) || exit 1
# Git Bash's mktemp hands back a Windows drive prefix (C:\...), which the delete helpers on that host
# refuse; going through pwd gives the POSIX form there and changes nothing on Linux.
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
RUNDIR=$T/run; export ZTE_MESH_RUNDIR=$RUNDIR
LOGF=$T/log
pass=0; fail=0; AFTER_LOAD=''; LOADED=''
CHECK_PERMISSIONS=1
( umask 077; : > "$T/mode-check" ) || exit 1
mode_check=$(find "$T/mode-check" -type f -perm 0600) || exit 1
if [ -z "$mode_check" ]; then
	case "$(uname -s)" in
	MINGW*|MSYS*)
		CHECK_PERMISSIONS=0
		echo "SKIP config permissions: this filesystem cannot represent 0600; verify on Linux" ;;
	*) echo "FAIL permissions probe: expected 0600" >&2; exit 1 ;;
	esac
fi

logger() { for _a; do :; done; echo "$_a" >> "$LOGF"; }
date() { printf '%s\n' "${STUB_TZ:-+0000}"; }
# Track assigned names to avoid forking a pipeline to scan all shell variables.
config_load() {
	SECS=""; LOADED=''; local line cur="" n=0 k v t
	while IFS= read -r line; do
		line=${line#"${line%%[![:space:]]*}"}
		case "$line" in
		\#*) ;;
		config\ *)
			set -- $line; t=$2; cur=${3:-}; cur=${cur#\'}; cur=${cur%\'}
			[ -n "$cur" ] || { n=$((n + 1)); cur=cfg$n; }
			SECS="$SECS $cur"; eval "T_$cur=\$t"; LOADED="$LOADED T_$cur" ;;
		option\ *)
			line=${line#option }; k=${line%% *}; v=${line#* }
			# like libuci: a quoted value ends at its closing quote, an unquoted '#' starts a comment
			case "$v" in \'*) v=${v#\'}; v=${v%%\'*} ;; *) v=${v%%[[:space:]#]*} ;; esac
			eval "V_${cur}_$k=\$v"; LOADED="$LOADED V_${cur}_$k" ;;
		esac
	done < "$UCI"
	[ -z "$AFTER_LOAD" ] || eval "$AFTER_LOAD"
}
config_get() {
	local _s
	eval "_s=\${V_${2}_${3}+set}"
	if [ -n "$_s" ]; then eval "$1=\$V_${2}_${3}"; else eval "$1=\${4:-}"; fi
}
# libubox's spelling of a boolean: the words below are 1, everything else (including an absent option) is 0.
config_get_bool() {
	local _v
	config_get "$1" "$2" "$3" "${4:-}"
	eval "_v=\$$1"
	case $(echo "$_v" | tr 'A-Z' 'a-z') in
	1|on|true|yes|enabled) eval "$1=1" ;;
	*) eval "$1=0" ;;
	esac
}
config_foreach() {
	local s t
	for s in $SECS; do eval "t=\$T_$s"; [ "$t" = "$2" ] && "$1" "$s"; done
}
# uci stub for the child-role cases: WIRELESS_DB holds `wireless.<section>[.<option>]=<value>` lines, the same shape
# `uci show wireless` prints, so write_config can borrow the network this router already broadcasts.
WIRELESS_DB=''
uci() {
	[ "${1:-}" = -q ] && shift
	case "${1:-}" in
	show) [ -n "$WIRELESS_DB" ] && sort "$WIRELESS_DB" ;;
	get) [ -n "$WIRELESS_DB" ] || return 1
		sed -n "s/^$2=//p" "$WIRELESS_DB" | head -n 1 ;;
	*) return 1 ;;
	esac
}
. "$INIT"

# 0. The advertised model follows the radio generation; start_service passes radio_class's answer through this
#    mapping. The class itself needs /sys and iw, so only the mapping is a host-testable string table.
model_case() {
	local got
	got=$(model_for_radio "$1")
	if [ "$got" = "$2" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL model_for_radio($1) = '$got', want '$2'"
	fi
}
model_case none "ZXSLC SR1010"
model_case vht "ZXHN Z506"
model_case he "ZXHN E2631"
model_case eht "ZXHN E3710"
model_case unknown "ZXHN E2631"

# uci TEXT ... ; case NAME EXPECT-RC [expected output file text | -]
run() {
	local name="$1" want_rc="$2" expect="$3" rc got mode f
	rm -rf "$RUNDIR" "$LOGF"; : > "$LOGF"
	UCI=$T/uci; printf '%s\n' "$UCI_TEXT" > "$UCI"
	for _v in $LOADED; do unset "$_v"; done
	write_config; rc=$?
	if [ "$rc" != "$want_rc" ]; then
		echo "FAIL $name: rc=$rc want $want_rc ($(tail -1 "$LOGF"))"; fail=$((fail + 1)); return
	fi
	if [ "$want_rc" = 0 ]; then
		got=$(cat "$RUNDIR/config")
		if [ "$CHECK_PERMISSIONS" = 1 ]; then
			mode=$(find "$RUNDIR/config" -type f -perm 0600) && [ "$mode" = "$RUNDIR/config" ] || {
				echo "FAIL $name: config is not 0600"; fail=$((fail + 1)); return;
			}
		fi
		[ "$got" = "$expect" ] || { echo "FAIL $name: output differs"; echo "--- got"; echo "$got"; echo "--- want"; echo "$expect"; fail=$((fail + 1)); return; }
	else
		[ ! -e "$RUNDIR/config" ] || { echo "FAIL $name: a refused configuration still wrote the file"; fail=$((fail + 1)); return; }
		case "$(cat "$LOGF")" in *"$expect"*) ;; *) echo "FAIL $name: log lacks '$expect': $(tail -1 "$LOGF")"; fail=$((fail + 1)); return ;; esac
	fi
	for f in "$RUNDIR"/.config.*; do
		[ ! -e "$f" ] && [ ! -L "$f" ] || { echo "FAIL $name: temp file left behind"; fail=$((fail + 1)); return; }
	done
	[ "$want_rc" != 0 ] || [ -z "$DUMP_DIR" ] || cp "$RUNDIR/config" "$DUMP_DIR/$(printf %02d $((pass + 1)))-$name.conf"
	WIRELESS_DB=''
	pass=$((pass + 1))
}

BASE="config zte_mesh 'main'
	option mode 'zte'
	option wifi_24g_enabled '1'
	option wifi_24g_ssid 'home'
	option wifi_24g_key 'password-24'
	option wifi_5g_enabled '1'"

# 1. legacy file: byte-identical to what the previous init wrote (its old 'mode' option is ignored now)
UCI_TEXT="$BASE
	option wifi_5g_ssid 'home-5'
	option wifi_5g_key 'password-5g'"
run legacy 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=home-5
radio1.key=password-5g"
[ "$(cat "$RUNDIR/version_mode.host")" = 128 ] || { echo "FAIL legacy: version_mode.host"; fail=$((fail + 1)); }
[ "$(cat "$RUNDIR/restart.sig")" = "128 role=master local=off bridge=br-lan al_last= emulator= " ] || { echo "FAIL legacy: restart.sig='$(cat "$RUNDIR/restart.sig")'"; fail=$((fail + 1)); }

# 1b. the shipped template as it is (default name ImmortalWrt, default password Ztemesh2631!): bands synchronised (the
# factory baseline), so the 5 GHz band takes the 2.4 GHz name, key and security and the child's 5 GHz network is not renamed
TEMPLATE=$(dirname "$INIT")/zte-mesh.config
UCI_TEXT=$(cat "$TEMPLATE")
run template 0 "version_mode=128
radio0.enabled=1
radio0.ssid=ImmortalWrt
radio0.key=Ztemesh2631!
radio0.hidden=0
radio0.beacon_type=12
radio0.beacon_interval=100
radio0.channel=0
radio0.tx_power=20
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=12
radio1.beacon_interval=100
radio1.channel=0
radio1.tx_power=23
wifi.dual_band_sync=1
wifi.essid_suffix=-5G
mesh.v2=0
mesh.role=master
mlo.enable=1
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70"
# the same template without the sync line: an empty 5 GHz name is derived (not refused), which renames the 5 GHz network,
# and the 5 GHz band keeps its own security
UCI_TEXT=$(printf '%s\n' "$UCI_TEXT" | grep -v wifi_dual_band_sync)
run template-no-sync 0 "version_mode=128
radio0.enabled=1
radio0.ssid=ImmortalWrt
radio0.key=Ztemesh2631!
radio0.hidden=0
radio0.beacon_type=12
radio0.beacon_interval=100
radio0.channel=0
radio0.tx_power=20
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=6
radio1.beacon_interval=100
radio1.channel=0
radio1.tx_power=23
wifi.essid_suffix=-5G
mesh.v2=0
mesh.role=master
mlo.enable=1
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70"

# 2. OpenWrt spellings of booleans; without a mode option the VersionMode is the fixed 128 as well
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_enabled 'on'
	option wifi_24g_ssid 'a'
	option wifi_24g_key '12345678'
	option wifi_5g_enabled 'false'
	option wifi_5g_ssid 'b'
	option wifi_5g_key '87654321'"
run standard-bools 0 "version_mode=128
radio0.enabled=1
radio0.ssid=a
radio0.key=12345678
radio1.enabled=0
radio1.ssid=b
radio1.key=87654321"

# 3. synchronised bands: the 5 GHz name and key are not written, even if the UCI still holds old ones
UCI_TEXT="$BASE
	option wifi_dual_band_sync '1'
	option wifi_5g_ssid 'old-5'
	option wifi_5g_key 'old-key-5'"
run sync-on 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
wifi.dual_band_sync=1"

# 4. bands not synchronised, 5 GHz name derived (default suffix: nothing written), key follows 2.4 GHz
UCI_TEXT="$BASE
	option wifi_dual_band_sync '0'"
run derived-default 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
wifi.dual_band_sync=0"

# 5. custom suffix, own key
UCI_TEXT="$BASE
	option wifi_dual_band_sync '0'
	option wifi_5g_suffix '-5GHz'
	option wifi_5g_key 'password-5g'"
run derived-suffix 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.key=password-5g
wifi.dual_band_sync=0
wifi.essid_suffix=-5GHz"

# 6. derived name too long (29 + 3 = 32) / suffix errors / missing 2.4 GHz profile
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid '12345678901234567890123456789'
	option wifi_24g_key '12345678'
	option wifi_dual_band_sync '0'"
run derived-too-long 1 "derived 5 GHz name"
UCI_TEXT="$BASE
	option wifi_dual_band_sync '0'
	option wifi_5g_suffix '123456789'"
run suffix-too-long 1 "wifi_5g_suffix"
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid ''
	option wifi_24g_key '12345678'"
run no-ssid 1 "incomplete"
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'x'
	option wifi_24g_key 'short'"
run short-key 1 "incomplete"

# 7. control characters can not inject keys
NL=$(printf 'x\nradio0.enabled=0')
UCI_TEXT="$BASE
	option wifi_5g_ssid 'ok'"
UCI=$T/uci; rm -rf "$RUNDIR"
printf '%s\n' "$UCI_TEXT" > "$UCI"
AFTER_LOAD='V_main_wifi_24g_ssid=$NL'; : > "$LOGF"
if write_config; then echo "FAIL newline-ssid: accepted"; fail=$((fail + 1)); else pass=$((pass + 1)); fi
AFTER_LOAD='V_main_wifi_24g_key="password-24
radio0.enabled=0"'; rm -rf "$RUNDIR"
if write_config; then echo "FAIL newline-key: accepted"; fail=$((fail + 1)); else pass=$((pass + 1)); fi
AFTER_LOAD=''
[ ! -e "$RUNDIR/config" ] || { echo "FAIL newline-ssid: wrote the file"; fail=$((fail + 1)); }

# 8. every optional main option
UCI_TEXT="config zte_mesh 'main'
	option mode 'zte_compat'
	option wifi_24g_enabled '1'
	option wifi_24g_ssid 'cc'
	option wifi_24g_key '12345678'
	option wifi_24g_hidden '0'
	option wifi_24g_security 'sae-mixed'
	option wifi_24g_beacon_interval '100'
	option wifi_24g_channel '6'
	option wifi_24g_tx_power '20'
	option wifi_24g_acl_policy 'ban'
	option wifi_5g_enabled '1'
	option wifi_5g_hidden '1'
	option wifi_5g_security 'psk2'
	option wifi_5g_beacon_interval '200'
	option wifi_5g_channel '149'
	option wifi_5g_tx_power '30'
	option wifi_5g_acl_policy 'allow'
	option wifi_dual_band_sync '1'
	option mesh_compatible '0'
	# The retired key stays in the file: read as well it would emit a second mesh.v2 line (exact-match below), and
	# read instead of mesh_compatible it would emit mesh.v2=0. The expected output has mesh.v2=1 exactly once.
	option mesh_v2 '1'
	option mesh_role 'master'
	option apply_local '1'
	option mlo_enable '0'
	option band_steer '1'
	option band_steer_rssi_24g '-60'
	option band_steer_rssi_5g '-110'
	option roam_rssi_24g '-60'
	option roam_rssi_5g '-70'
	option roam_force '0'
	option ban_minutes '30'
	option extra_slots '1'
	option backhaul 'multi_ap'
	option wifi_2g 'radio2'
	option wifi_5g 'radio1'"
run all-options 0 "version_mode=128
radio0.enabled=1
radio0.ssid=cc
radio0.key=12345678
radio0.hidden=0
radio0.beacon_type=12
radio0.beacon_interval=100
radio0.channel=6
radio0.tx_power=20
radio0.acl_policy=ban
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=12
radio1.beacon_interval=200
radio1.channel=149
radio1.tx_power=30
radio1.acl_policy=ban
wifi.dual_band_sync=1
mesh.v2=1
mesh.role=master
mlo.enable=0
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70
ban.minutes=30"
[ "$(cat "$RUNDIR/restart.sig")" = "band_steer.enabled band_steer.rssi24 band_steer.rssi5 mesh.role mlo.enable roam.force roam.rssi24 roam.rssi5 wifi.dual_band_sync 128 role=master local=on backhaul=multi_ap extra_slots=1 pin2g=radio2 pin5g=radio1 bridge=br-lan al_last= emulator= " ] ||
	{ echo "FAIL all-options: restart.sig='$(cat "$RUNDIR/restart.sig")'"; fail=$((fail + 1)); }
# 8b. without dual-band sync the 5 GHz band keeps its own hidden flag, security and MAC filter policy
UCI_TEXT=$(printf '%s\n' "$UCI_TEXT" | sed "s/wifi_dual_band_sync '1'/wifi_dual_band_sync '0'/")
run all-options-no-sync 0 "version_mode=128
radio0.enabled=1
radio0.ssid=cc
radio0.key=12345678
radio0.hidden=0
radio0.beacon_type=12
radio0.beacon_interval=100
radio0.channel=6
radio0.tx_power=20
radio0.acl_policy=ban
radio1.enabled=1
radio1.hidden=1
radio1.beacon_type=3
radio1.beacon_interval=200
radio1.channel=149
radio1.tx_power=30
radio1.acl_policy=allow
wifi.dual_band_sync=0
mesh.v2=1
mesh.role=master
mlo.enable=0
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70
ban.minutes=30"

# 8c. mesh_role slave: the child side of the same switch, emitted verbatim (the service reads it)
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
	option mesh_role 'slave'"
run role-slave 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
mesh.role=slave"
grep -q '^mesh.role=slave$' "$RUNDIR/config" || { echo "FAIL role-slave: mesh.role not written"; fail=$((fail + 1)); }
grep -q 'mesh.role' "$RUNDIR/restart.sig" || { echo "FAIL role-slave: restart.sig lacks mesh.role"; fail=$((fail + 1)); }
# The child's plan is published by the service from the rows the M2 wrote; write_config must not produce one, and must
# not remove the one a running service is using.
AFTER_LOAD='mkdir -p "$RUNDIR"; printf "sentinel\n" > "$RUNDIR/wlan-plan"'
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
	option mesh_role 'slave'"
run role-slave-keeps-plan 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
mesh.role=slave"
AFTER_LOAD=''
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = sentinel ] ||
	{ echo "FAIL role-slave-keeps-plan: write_config overwrote the service's plan"; fail=$((fail + 1)); }

# 8g. controller that also broadcasts: master + apply_local=1. The daemon hears master -- not the adaptive triple it
# used to be told, so nothing is elected and Mode is written once -- and the host side gets a plan for its own radios,
# which a controller-only router never produces. The 5 GHz row spells out what the daemon derives for itself.
UCI_TEXT="$BASE
	option mesh_role 'master'
	option apply_local '1'"
run role-main 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = "# zte-mesh local Wi-Fi plan (this router broadcasts). Generated by the service; applied by wlan-sync.
rows=2
0.radio=0
0.slot=0
0.ssid=home
0.key=password-24
0.enabled=1
0.access=32
1.radio=1
1.slot=0
1.ssid=home-5G
1.key=password-24
1.enabled=1
1.access=32" ] || { echo "FAIL role-main: plan differs"; sed 's/^/    /' "$RUNDIR/wlan-plan"; fail=$((fail + 1)); }
[ -z "$(find "$RUNDIR/wlan-plan" -perm -077 2>/dev/null)" ] || { echo "FAIL role-main: plan is not 0600"; fail=$((fail + 1)); }
ls "$RUNDIR"/.wlan-plan.* >/dev/null 2>&1 && { echo "FAIL role-main: temp plan left behind"; fail=$((fail + 1)); }

# 8g2. broadcasting controller + extra_slots: the extra SSID slots this router broadcasts are appended to its own
# plan, so one script applies them on either role -- on a child the rows come from the M2, here from 'config zte_mesh_bss'.
# Slot 1 and 2 are fronthaul (MapAccessAttr 32), slot 3 is the factory's hidden backhaul BSS (64).
UCI_TEXT="$BASE
	option mesh_role 'master'
	option apply_local '1'
	option extra_slots '1'
config zte_mesh_bss 'guest24'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'guest'
	option key 'guest-password'
config zte_mesh_bss 'spare24'
	option band '24g'
	option slot '2'
	option enabled '0'
	option ssid 'spare'
	option key 'spare-password'
config zte_mesh_bss 'bh24'
	option band '24g'
	option slot '3'
	option enabled '1'
	option ssid 'ZMESH-bh'
	option key 'backhaul-password'
	option hidden '1'
	option security 'psk2'"
run role-main-slots 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master
radio0.bss1.enabled=1
radio0.bss1.ssid=guest
radio0.bss1.key=guest-password
radio0.bss2.enabled=0
radio0.bss2.ssid=spare
radio0.bss2.key=spare-password
radio0.bss3.enabled=1
radio0.bss3.ssid=ZMESH-bh
radio0.bss3.key=backhaul-password
radio0.bss3.hidden=1
radio0.bss3.beacon_type=3"
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = "# zte-mesh local Wi-Fi plan (this router broadcasts). Generated by the service; applied by wlan-sync.
rows=5
0.radio=0
0.slot=0
0.ssid=home
0.key=password-24
0.enabled=1
0.access=32
1.radio=1
1.slot=0
1.ssid=home-5G
1.key=password-24
1.enabled=1
1.access=32
2.radio=0
2.slot=1
2.ssid=guest
2.key=guest-password
2.enabled=1
2.access=32
3.radio=0
3.slot=2
3.ssid=spare
3.key=spare-password
3.enabled=0
3.access=32
4.radio=0
4.slot=3
4.ssid=ZMESH-bh
4.key=backhaul-password
4.enabled=1
4.access=64
4.hidden=1
4.beacon_type=3" ] || { echo "FAIL role-main-slots: plan differs"; sed 's/^/    /' "$RUNDIR/wlan-plan"; fail=$((fail + 1)); }
ls "$RUNDIR"/.bss-plan.* >/dev/null 2>&1 && { echo "FAIL role-main-slots: the slot fragment was left behind"; fail=$((fail + 1)); }

# 8g3. broadcasting controller WITHOUT extra_slots: the same configuration produces the two main rows only. This is
# the option that keeps the old behaviour, so it must be the one that decides, not the presence of the sections.
UCI_TEXT="$BASE
	option mesh_role 'master'
	option apply_local '1'
config zte_mesh_bss 'guest24'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'guest'
	option key 'guest-password'"
run role-main-no-slots 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master
radio0.bss1.enabled=1
radio0.bss1.ssid=guest
radio0.bss1.key=guest-password"
grep -q '^2\.radio=' "$RUNDIR/wlan-plan" 2>/dev/null &&
	{ echo "FAIL role-main-no-slots: a slot row reached the plan with extra_slots unset"; fail=$((fail + 1)); }

# 8g4. a child keeps the plan the service published, extra_slots or not: its rows come from the M2, and this role is
# the one the option changes nothing about.
AFTER_LOAD='mkdir -p "$RUNDIR"; printf "sentinel\n" > "$RUNDIR/wlan-plan"'
UCI_TEXT="$BASE
	option mesh_role 'slave'
	option extra_slots '1'"
run role-slave-slots-keeps-plan 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=slave"
AFTER_LOAD=''
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = sentinel ] ||
	{ echo "FAIL role-slave-slots-keeps-plan: write_config overwrote the service's plan"; fail=$((fail + 1)); }
ls "$RUNDIR"/.bss-plan.* >/dev/null 2>&1 && { echo "FAIL role-slave-slots-keeps-plan: a child wrote a slot fragment"; fail=$((fail + 1)); }

# 8g5. extra_slots is validated like every other switch, and a bad value does not silently mean off.
UCI_TEXT="$BASE
	option mesh_role 'master'
	option apply_local '1'
	option extra_slots '2'"
run role-main-slots-bad 1 "extra_slots must be 0 or 1"

# 8h. broadcasting controller with synchronised bands: the 5 GHz row is the 2.4 GHz row, hidden flag and security
# included, and an empty 5 GHz pair must not leak into it
UCI_TEXT="$BASE
	option wifi_5g_ssid 'ignored'
	option wifi_5g_key 'ignored-key'
	option wifi_dual_band_sync '1'
	option wifi_24g_hidden '1'
	option wifi_24g_security 'sae'
	option mesh_role 'master'
	option apply_local '1'"
run role-main-sync 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio0.hidden=1
radio0.beacon_type=10
radio1.enabled=1
radio1.hidden=1
radio1.beacon_type=10
wifi.dual_band_sync=1
mesh.role=master"
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = "# zte-mesh local Wi-Fi plan (this router broadcasts). Generated by the service; applied by wlan-sync.
rows=2
0.radio=0
0.slot=0
0.ssid=home
0.key=password-24
0.enabled=1
0.access=32
0.hidden=1
0.beacon_type=10
1.radio=1
1.slot=0
1.ssid=home
1.key=password-24
1.enabled=1
1.access=32
1.hidden=1
1.beacon_type=10" ] || { echo "FAIL role-main-sync: plan differs"; sed 's/^/    /' "$RUNDIR/wlan-plan"; fail=$((fail + 1)); }

# 8i. controller only (master, apply_local off): the profile is handed out and this router's radios are left alone,
# so no plan -- and a plan left behind by an earlier role is removed instead of being applied by a wlan-sync that
# no longer runs. The restart signature changes with either decision, because both start or stop an instance.
AFTER_LOAD='mkdir -p "$RUNDIR"; printf "sentinel\n" > "$RUNDIR/wlan-plan"'
UCI_TEXT="$BASE
	option mesh_role 'master'"
run role-master 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
AFTER_LOAD=''
[ ! -e "$RUNDIR/wlan-plan" ] || { echo "FAIL role-master: a plan was written for the controller-only role"; fail=$((fail + 1)); }

# 8j. the two legacy names of the broadcasting controller, `main` and `auto`: same plan, same wire value, so an
# upgraded configuration keeps working without the user touching the form. They only ever mean apply_local=1 when
# the option itself is not written; written explicitly, the option wins.
UCI_TEXT="$BASE
	option mesh_role 'auto'"
run role-auto-legacy 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
grep -q '^1.ssid=home-5G$' "$RUNDIR/wlan-plan" 2>/dev/null ||
	{ echo "FAIL role-auto-legacy: no plan for the legacy name of the broadcasting controller"; fail=$((fail + 1)); }
UCI_TEXT="$BASE
	option mesh_role 'main'"
run role-main-legacy 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
grep -q '^1.ssid=home-5G$' "$RUNDIR/wlan-plan" 2>/dev/null ||
	{ echo "FAIL role-main-legacy: no plan for the other legacy name"; fail=$((fail + 1)); }

# 8k. restart.sig is the decision, not the key name: the same master flipping between broadcasting and controller-only
# has to look different to reload_service, or the wlan-sync instance would be left running (or never started) until
# somebody restarted the service by hand.
UCI_TEXT="$BASE
	option mesh_role 'master'
	option apply_local '1'"
run role-local-sig 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
[ "$(cat "$RUNDIR/restart.sig")" = "mesh.role 128 role=master local=on backhaul=off extra_slots=0 pin2g= pin5g= bridge=br-lan al_last= emulator= " ] ||
	{ echo "FAIL role-local-sig: restart.sig='$(cat "$RUNDIR/restart.sig")'"; fail=$((fail + 1)); }

# 8l. a child ignores apply_local: its plan is the controller's, so the flag must not resurrect a local plan here,
# and the signature still says the controller half is off.
AFTER_LOAD='mkdir -p "$RUNDIR"; printf "sentinel\n" > "$RUNDIR/wlan-plan"'
UCI_TEXT="$BASE
	option mesh_role 'slave'
	option apply_local '1'"
run role-slave-local-keeps-plan 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=slave"
AFTER_LOAD=''
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = sentinel ] ||
	{ echo "FAIL role-slave-local-keeps-plan: apply_local wrote a plan on a child"; fail=$((fail + 1)); }
[ "$(cat "$RUNDIR/restart.sig")" = "mesh.role 128 role=slave local=off backhaul=off extra_slots=0 pin2g= pin5g= bridge=br-lan al_last= emulator= " ] ||
	{ echo "FAIL role-slave-local: restart.sig='$(cat "$RUNDIR/restart.sig")'"; fail=$((fail + 1)); }

# 8n. the applier reads four options straight out of UCI, and none of them belongs in the signature for the reason the
# role pair does: they change neither which instances exist nor the plan, only what wlan-sync writes *from* the plan it
# is given -- and that watcher reacts to plan content alone. So saving any of them has to look like a restart, on either
# of the two roles that can build something. On the third (a controller that applies nothing here) it must not, because
# those values have no effect there and the service already says so at start.
# runs one role/option shape and leaves its signature in SIG (not a subshell: `run` counts the case where it is)
sig_of() {
	UCI_TEXT="$BASE
	option mesh_role '$1'
	$2"
	run "sig-$3" 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=$1"
	SIG=$(cat "$RUNDIR/restart.sig")
}
sig_of master "option apply_local '1'" broadcast-off; BROADCAST=$SIG
sig_of master "option apply_local '1'
	option backhaul 'multi_ap'" broadcast-on; BROADCAST_BH=$SIG
sig_of master "option apply_local '1'
	option backhaul 'off'" broadcast-explicit-off; BROADCAST_OFF=$SIG
sig_of slave "" child-off; CHILD=$SIG
sig_of slave "option backhaul 'multi_ap'" child-on; CHILD_BH=$SIG
sig_of master "option backhaul 'multi_ap'" only-on; ONLY=$SIG
sig_of master "" only-off; ONLY_PLAIN=$SIG
[ "$BROADCAST" != "$BROADCAST_BH" ] ||
	{ echo "FAIL backhaul-sig: a broadcasting master kept the same signature ($BROADCAST)"; fail=$((fail + 1)); }
[ "$CHILD" != "$CHILD_BH" ] ||
	{ echo "FAIL backhaul-sig: a child kept the same signature ($CHILD)"; fail=$((fail + 1)); }
case "$BROADCAST_BH" in *'backhaul=multi_ap '*) ;; *) echo "FAIL backhaul-sig: multi_ap missing on the master ($BROADCAST_BH)"; fail=$((fail + 1)) ;; esac
case "$CHILD_BH" in *'backhaul=multi_ap '*) ;; *) echo "FAIL backhaul-sig: multi_ap missing on the child ($CHILD_BH)"; fail=$((fail + 1)) ;; esac
# writing the default is the same decision as not writing it: the signature must not churn over a save that changed nothing
[ "$BROADCAST" = "$BROADCAST_OFF" ] ||
	{ echo "FAIL backhaul-sig: backhaul=off differs from an unset mode ('$BROADCAST' -> '$BROADCAST_OFF')"; fail=$((fail + 1)); }
[ "$ONLY" = "$ONLY_PLAIN" ] ||
	{ echo "FAIL backhaul-sig: a controller-only box restarts for a value it ignores ('$ONLY_PLAIN' -> '$ONLY')"; fail=$((fail + 1)); }
case "$ONLY" in *backhaul*) echo "FAIL backhaul-sig: the role that runs no applier carries the mode ($ONLY)"; fail=$((fail + 1)) ;; esac
sig_of master "option apply_local '1'
	option extra_slots '1'" slots-on; SLOTS=$SIG
sig_of master "option apply_local '1'
	option extra_slots '0'" slots-explicit-off; SLOTS_OFF=$SIG
sig_of slave "option wifi_2g 'radio1'" child-pin; CHILD_PIN=$SIG
sig_of master "option extra_slots '1'
	option backhaul 'multi_ap'
	option wifi_2g 'radio1'
	option wifi_5g 'radio0'" only-everything; ONLY_ALL=$SIG
[ "$BROADCAST" != "$SLOTS" ] ||
	{ echo "FAIL applier-sig: a broadcasting master kept the same signature without the extra slots ($BROADCAST)"; fail=$((fail + 1)); }
[ "$CHILD" != "$CHILD_PIN" ] ||
	{ echo "FAIL applier-sig: a child kept the same signature with a radio pin ($CHILD)"; fail=$((fail + 1)); }
case "$SLOTS" in *'extra_slots=1 '*) ;; *) echo "FAIL applier-sig: extra_slots missing ($SLOTS)"; fail=$((fail + 1)) ;; esac
case "$CHILD_PIN" in *'pin2g=radio1 '*) ;; *) echo "FAIL applier-sig: the pin missing on a child ($CHILD_PIN)"; fail=$((fail + 1)) ;; esac
[ "$BROADCAST" = "$SLOTS_OFF" ] ||
	{ echo "FAIL applier-sig: extra_slots=0 differs from an unset switch ('$BROADCAST' -> '$SLOTS_OFF')"; fail=$((fail + 1)); }
[ "$ONLY_ALL" = "$ONLY_PLAIN" ] ||
	{ echo "FAIL applier-sig: a controller-only box restarts for values it ignores ('$ONLY_PLAIN' -> '$ONLY_ALL')"; fail=$((fail + 1)); }
case "$ONLY_ALL" in *extra_slots*|*pin*|*backhaul*) echo "FAIL applier-sig: the role that runs no applier carries them ($ONLY_ALL)"; fail=$((fail + 1)) ;; esac

# 8o. the three options start_service() bakes into the instances: the bridge `attach` and `mesh-search` are handed
# (and the LAN MAC the AL MAC is derived from), that derivation's override, and the emulator decision. None of them
# reaches the config file or the plan, so nothing inside the sandbox sees a change and no instance is added or
# removed -- without a token here a save on the page sits inert until somebody restarts by hand. Every role reads
# all three, including the controller-only one that deliberately carries none of the applier tokens above.
sig_of master "option bridge 'br-guest'" only-bridge; ONLY_BRIDGE=$SIG
sig_of master "option bridge 'br-lan'" only-bridge-default; ONLY_BRIDGE_DEFAULT=$SIG
sig_of slave "option al_last 'e3'" child-al; CHILD_AL=$SIG
sig_of master "option emulator 'none'" only-emulator; ONLY_EMULATOR=$SIG
[ "$ONLY_PLAIN" != "$ONLY_BRIDGE" ] ||
	{ echo "FAIL instance-sig: a controller-only box kept the same signature after the bridge moved ($ONLY_PLAIN)"; fail=$((fail + 1)); }
[ "$CHILD" != "$CHILD_AL" ] ||
	{ echo "FAIL instance-sig: a child kept the same signature after the AL MAC override changed ($CHILD)"; fail=$((fail + 1)); }
[ "$ONLY_PLAIN" != "$ONLY_EMULATOR" ] ||
	{ echo "FAIL instance-sig: the emulator decision is missing from the signature ($ONLY_PLAIN)"; fail=$((fail + 1)); }
[ "$ONLY_PLAIN" = "$ONLY_BRIDGE_DEFAULT" ] ||
	{ echo "FAIL instance-sig: bridge=br-lan differs from an unset bridge ('$ONLY_PLAIN' -> '$ONLY_BRIDGE_DEFAULT')"; fail=$((fail + 1)); }

# 8p. reload_service() is the path a save on the page actually takes, and it is not start_service(): the signature
# decides whether procd restarts. `enabled` is the page's off switch and used to be invisible here -- write_config()
# describes the profile rather than whether it runs, so it succeeds for a disabled service, the two signatures match
# and the answer was "nothing to do" while the controller kept running. rc.common and procd are replaced by three
# recorders; what is graded is which of them reload_service() called, not what they would have done.
ACTIONS=''; RUNNING=1
stop() { ACTIONS="$ACTIONS stop"; }
restart() { ACTIONS="$ACTIONS restart"; }
procd_running() { [ "$RUNNING" = 1 ]; }
# reload_case NAME OLD-OPTIONS NEW-OPTIONS WANT-ACTIONS [WANT-RC] -- write the old profile, then save the new one.
reload_case() {
	local name="$1" rc
	ACTIONS=''
	rm -rf "$RUNDIR"
	UCI=$T/uci
	printf '%s\n' "$BASE
$2" > "$UCI"
	for _v in $LOADED; do unset "$_v"; done
	write_config || { fail=$((fail + 1)); echo "FAIL reload $name: the baseline profile was refused"; return; }
	printf '%s\n' "$BASE
$3" > "$UCI"
	for _v in $LOADED; do unset "$_v"; done
	reload_service; rc=$?
	if [ "$rc" != "${5:-0}" ]; then
		fail=$((fail + 1)); echo "FAIL reload $name: rc=$rc want ${5:-0}"; return
	fi
	if [ "$ACTIONS" != "$4" ]; then
		fail=$((fail + 1)); echo "FAIL reload $name: called '$ACTIONS', want '$4'"; return
	fi
	pass=$((pass + 1))
}
reload_case unchanged "option enabled '1'" "option enabled '1'" ''
reload_case turned-off "option enabled '1'" "option enabled '0'" ' stop'
# The off switch has to work even when the profile left behind cannot be saved: the refusal path below answers 1,
# and a page that cannot switch the service off because of an unrelated field is the worse failure.
reload_case turned-off-bad-profile "option enabled '1'" "option enabled '0'
	option wifi_24g_enabled '2'" ' stop'
reload_case bridge "option enabled '1'" "option enabled '1'
	option bridge 'br-guest'" ' restart'
reload_case al-last "option enabled '1'" "option enabled '1'
	option al_last 'e3'" ' restart'
reload_case emulator "option enabled '1'" "option enabled '1'
	option emulator 'none'" ' restart'
reload_case refused "option enabled '1'" "option enabled '1'
	option wifi_24g_enabled '2'" ' stop' 1
# A signature that says nothing changed is only an answer while the daemon is actually up.
RUNNING=0
reload_case not-running "option enabled '1'" "option enabled '1'" ' restart'
# Turning it back on: a stopped service has no signature (stop_service removes it), so the comparison differs and
# procd is asked to restart -- which is stop + start, and start_service() reads enabled again.
ACTIONS=''
rm -rf "$RUNDIR"
printf '%s\n' "$BASE
	option enabled '1'" > "$UCI"
for _v in $LOADED; do unset "$_v"; done
reload_service
if [ "$ACTIONS" = ' restart' ]; then pass=$((pass + 1)); else
	fail=$((fail + 1)); echo "FAIL reload turned-on: called '$ACTIONS', want ' restart'"; fi
RUNNING=1

# 8m. written explicitly, apply_local wins over the legacy name: `main` with 0 is a controller that was told to keep
# its own radios alone.
AFTER_LOAD='mkdir -p "$RUNDIR"; printf "sentinel\n" > "$RUNDIR/wlan-plan"'
UCI_TEXT="$BASE
	option mesh_role 'main'
	option apply_local '0'"
run role-main-local-off 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=master"
AFTER_LOAD=''
[ ! -e "$RUNDIR/wlan-plan" ] || { echo "FAIL role-main-local-off: an explicit 0 was ignored"; fail=$((fail + 1)); }

# 8d. child role: the seed is the network this router is broadcasting, not the profile the page still carries.
#
# The fixture is the lab's ax3600 as /etc/config/wireless describes it: a spare PCIe 5 GHz card (radio0, switched
# off and still named after the factory test network), the SoC's 5 GHz radio (radio1) and the 2.4 GHz radio (radio2).
# The declared profile below is the one the page leaves behind -- and seeding from it is what renamed the air twice
# on every restart of a child (the declared name, then the controller's when its M2 landed).
AX3600=$T/ax3600
printf '%s\n' \
	'wireless.radio0=wifi-device' 'wireless.radio0.band=5g' 'wireless.radio0.disabled=1' \
	'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' 'wireless.radio1.disabled=0' \
	'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' 'wireless.radio2.disabled=0' \
	'wireless.default_radio0=wifi-iface' 'wireless.default_radio0.device=radio0' 'wireless.default_radio0.mode=ap' \
	'wireless.default_radio0.ssid=ZTEMESHCTRL' 'wireless.default_radio0.key=00000000' \
	'wireless.default_radio0.encryption=sae-mixed' 'wireless.default_radio0.disabled=1' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' 'wireless.default_radio1.mode=ap' \
	'wireless.default_radio1.ssid=FRESHR10-5G' 'wireless.default_radio1.key=Ztemesh2631!' \
	'wireless.default_radio1.encryption=psk-mixed' 'wireless.default_radio1.disabled=0' 'wireless.default_radio1.hidden=0' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' 'wireless.default_radio2.mode=ap' \
	'wireless.default_radio2.ssid=FRESHR10' 'wireless.default_radio2.key=Ztemesh2631!' \
	'wireless.default_radio2.encryption=sae-mixed' 'wireless.default_radio2.disabled=0' 'wireless.default_radio2.hidden=0' \
	> "$AX3600"
SLAVE_UCI="config zte_mesh 'main'
	option wifi_24g_enabled '1'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_24g_security 'psk2'
	option wifi_5g_enabled '1'
	option wifi_dual_band_sync '1'
	option mesh_role 'slave'"
WIRELESS_DB=$AX3600
UCI_TEXT="$SLAVE_UCI"
run role-slave-borrow 0 "version_mode=128
radio0.enabled=1
radio0.ssid=FRESHR10
radio0.key=Ztemesh2631!
radio0.hidden=0
radio0.beacon_type=12
radio1.enabled=1
radio1.ssid=FRESHR10-5G
radio1.key=Ztemesh2631!
radio1.hidden=0
radio1.beacon_type=6
wifi.dual_band_sync=0
mesh.role=slave"
grep -q 'starting from the network this router broadcasts (2g 5g)' "$LOGF" ||
	{ echo "FAIL role-slave-borrow: the borrowed bands are not logged"; fail=$((fail + 1)); }
grep -q 'wifi_24g_ssid is not the seed' "$LOGF" ||
	{ echo "FAIL role-slave-borrow: the discarded profile is not logged"; fail=$((fail + 1)); }

# 8d-2. the spare card is not the air: borrowing from radio0 would put the factory test network in the seed, and the
# synchronised bands are a claim about what this router broadcasts -- two different pairs on the two radios have to
# be spelled out, or the daemon derives one name for both and the 5 GHz band gets renamed on the next restart.
[ "$(grep -c 'ZTEMESHCTRL' "$RUNDIR/config")" = 0 ] ||
	{ echo "FAIL role-slave-borrow: the switched-off card was borrowed"; fail=$((fail + 1)); }

# 8d-3. both bands broadcasting the same pair: the seed keeps the bands synchronised and writes no 5 GHz name,
# which is what lets the daemon derive the row and wlan-sync find nothing to change.
SYNCED=$T/synced
printf '%s\n' \
	'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' \
	'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' 'wireless.default_radio1.mode=ap' \
	'wireless.default_radio1.ssid=SAMEAP' 'wireless.default_radio1.key=samepassword' \
	'wireless.default_radio1.encryption=psk-mixed' 'wireless.default_radio1.hidden=0' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' 'wireless.default_radio2.mode=ap' \
	'wireless.default_radio2.ssid=SAMEAP' 'wireless.default_radio2.key=samepassword' \
	'wireless.default_radio2.encryption=psk-mixed' 'wireless.default_radio2.hidden=0' \
	> "$SYNCED"
WIRELESS_DB=$SYNCED
UCI_TEXT="config zte_mesh 'main'
	option mesh_role 'slave'"
run role-slave-borrow-same-pair 0 "version_mode=128
radio0.enabled=1
radio0.ssid=SAMEAP
radio0.key=samepassword
radio0.hidden=0
radio0.beacon_type=6
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=6
wifi.dual_band_sync=1
mesh.role=slave"

# 8d-3b. the bands are synchronised because the air says so, not because the page was left that way: a child that
# declares sync=0 and broadcasts one pair on both radios gets sync=1, and the 5 GHz name is not spelled out --
# the daemon derives the row from the 2.4 GHz one, which is what the air already is.
WIRELESS_DB=$SYNCED
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_dual_band_sync '0'
	option mesh_role 'slave'"
run role-slave-borrow-same-pair-derived 0 "version_mode=128
radio0.enabled=1
radio0.ssid=SAMEAP
radio0.key=samepassword
radio0.hidden=0
radio0.beacon_type=6
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=6
wifi.dual_band_sync=1
mesh.role=slave"

# 8d-3c. only the 2.4 GHz band readable and the page says the bands are synchronised: the 5 GHz row takes the
# borrowed hidden flag and security rather than the declared ones, because the daemon derives that band's profile
# from the 2.4 GHz side -- writing the declared security there would describe a network the seed does not use.
HALFREAD=$T/halfread
printf '%s\n' 'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' \
	'wireless.default_radio1.mode=ap' 'wireless.default_radio1.ssid=WEP5G' \
	'wireless.default_radio1.key=secret123' 'wireless.default_radio1.encryption=wep' >> "$HALFREAD"
printf '%s\n' 'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' \
	'wireless.default_radio2.mode=ap' 'wireless.default_radio2.ssid=SAMEAP' \
	'wireless.default_radio2.key=samepassword' 'wireless.default_radio2.encryption=psk-mixed' \
	'wireless.default_radio2.hidden=0' >> "$HALFREAD"
WIRELESS_DB=$HALFREAD
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_24g_security 'psk2'
	option wifi_dual_band_sync '1'
	option mesh_role 'slave'"
run role-slave-borrow-synced-one-band 0 "version_mode=128
radio0.enabled=1
radio0.ssid=SAMEAP
radio0.key=samepassword
radio0.hidden=0
radio0.beacon_type=6
radio1.enabled=1
radio1.hidden=0
radio1.beacon_type=6
wifi.dual_band_sync=1
mesh.role=slave"

# A board with two live radios on one band is a guess until the user pins it (the ax3600 without its spare card
# switched off). The 2.4 GHz seed still comes from the air; the 5 GHz band is left to the declared profile because
# nothing was read from it -- and a field the interface does not carry (hidden) is not invented either.
TWOLIVE=$T/twolive
printf '%s\n' \
	'wireless.radio0=wifi-device' 'wireless.radio0.band=5g' \
	'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' \
	'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' \
	'wireless.default_radio0=wifi-iface' 'wireless.default_radio0.device=radio0' 'wireless.default_radio0.mode=ap' \
	'wireless.default_radio0.ssid=SPARE5G' 'wireless.default_radio0.key=sparepassword' \
	'wireless.default_radio0.encryption=psk2' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' 'wireless.default_radio1.mode=ap' \
	'wireless.default_radio1.ssid=PINNED5G' 'wireless.default_radio1.key=pinnedpassword' \
	'wireless.default_radio1.encryption=sae' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' 'wireless.default_radio2.mode=ap' \
	'wireless.default_radio2.ssid=HOME2G' 'wireless.default_radio2.key=home2gpassword' \
	'wireless.default_radio2.encryption=psk-mixed' 'wireless.default_radio2.hidden=0' \
	> "$TWOLIVE"
WIRELESS_DB=$TWOLIVE
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_24g_security 'psk2'
	option mesh_role 'slave'"
run role-slave-borrow-two-live 0 "version_mode=128
radio0.enabled=1
radio0.ssid=HOME2G
radio0.key=home2gpassword
radio0.hidden=0
radio0.beacon_type=6
radio1.enabled=1
mesh.role=slave"
# …and the same router with the pin set names the device the applier will write, so the 5 GHz seed comes from it.
WIRELESS_DB=$TWOLIVE
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_24g_security 'psk2'
	option wifi_5g 'radio1'
	option mesh_role 'slave'"
run role-slave-borrow-pinned 0 "version_mode=128
radio0.enabled=1
radio0.ssid=HOME2G
radio0.key=home2gpassword
radio0.hidden=0
radio0.beacon_type=6
radio1.enabled=1
radio1.ssid=PINNED5G
radio1.key=pinnedpassword
radio1.beacon_type=10
wifi.dual_band_sync=0
mesh.role=slave"

# 8d-4. a band whose only radio is switched off broadcasts nothing, so there is no network to borrow from it: the
# declared profile stays in front of that band instead of a seed that would claim the band is up.
RADIOOFF=$T/radiooff
printf '%s\n' 'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' 'wireless.radio1.disabled=1' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' 'wireless.default_radio1.mode=ap' \
	'wireless.default_radio1.ssid=DEAD5G' 'wireless.default_radio1.key=deadpassword' \
	'wireless.default_radio1.encryption=psk2' > "$RADIOOFF"
printf '%s\n' 'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' \
	'wireless.default_radio2.mode=ap' 'wireless.default_radio2.ssid=HOME2G' \
	'wireless.default_radio2.key=home2gpassword' 'wireless.default_radio2.encryption=psk2' >> "$RADIOOFF"
WIRELESS_DB=$RADIOOFF
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option wifi_5g_ssid 'declared-5g'
	option wifi_5g_key 'declaredpassword'
	option mesh_role 'slave'"
run role-slave-borrow-radio-off 0 "version_mode=128
radio0.enabled=1
radio0.ssid=HOME2G
radio0.key=home2gpassword
radio0.beacon_type=3
radio1.enabled=1
radio1.ssid=declared-5g
radio1.key=declaredpassword
mesh.role=slave"

# 8d-5. a network that cannot be read cleanly is not borrowed: an open (keyless) 2.4 GHz and a 5 GHz whose security
# the service file has no word for leave the declared profile in charge.
WIRELESS_DB=$T/unreadable
printf '%s\n' 'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' \
	'wireless.default_radio2=wifi-iface' 'wireless.default_radio2.device=radio2' \
	'wireless.default_radio2.mode=ap' 'wireless.default_radio2.ssid=OPEN2G' 'wireless.default_radio2.encryption=none' \
	'wireless.radio1=wifi-device' 'wireless.radio1.band=5g' \
	'wireless.default_radio1=wifi-iface' 'wireless.default_radio1.device=radio1' \
	'wireless.default_radio1.mode=ap' 'wireless.default_radio1.ssid=WEP5G' \
	'wireless.default_radio1.key=secret123' 'wireless.default_radio1.encryption=wep' > "$WIRELESS_DB"
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_ssid 'ImmortalWrt'
	option wifi_24g_key 'labbpass123'
	option mesh_role 'slave'"
run role-slave-borrow-unreadable 0 "version_mode=128
radio0.enabled=1
radio0.ssid=ImmortalWrt
radio0.key=labbpass123
radio1.enabled=1
mesh.role=slave"

# 8d-6. the seed is a child's problem: a controller broadcasts the profile it hands out, so the air it happens to
# carry is never read into the service file.
WIRELESS_DB=$AX3600
UCI_TEXT="$BASE
	option wifi_5g_ssid 'home-5'
	option wifi_5g_key 'password-5g'"
run master-ignores-air 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=home-5
radio1.key=password-5g"

# 8e. child role, no profile and nothing to borrow: refuses, and the message says what a child actually needs
WIRELESS_DB=$T/no-interfaces
printf '%s\n' 'wireless.radio2=wifi-device' 'wireless.radio2.band=2g' > "$WIRELESS_DB"
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_enabled '1'
	option wifi_5g_enabled '1'
	option mesh_role 'slave'"
run role-slave-no-profile 1 "child role: no Wi-Fi profile and none to borrow"

# 8f. the master role keeps the old refusal: a controller with no network to hand out is a mistake, not a default
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_enabled '1'
	option wifi_5g_enabled '1'"
run master-no-profile 1 "Wi-Fi profile incomplete"

# 9. bad optional values name the option
for bad in "wifi_24g_hidden 2" "wifi_24g_security wep" "wifi_24g_beacon_interval 19" "wifi_24g_beacon_interval 1001" "wifi_24g_channel 14" \
	"wifi_5g_channel 37" "wifi_24g_tx_power 31" "wifi_24g_tx_power -1" "wifi_24g_acl_policy deny" "mesh_compatible 2" "mesh_role child" "apply_local 2" \
	"mlo_enable x" "band_steer maybe" "roam_force 5" "wifi_dual_band_sync 2" "band_steer_rssi_24g -29" "roam_rssi_5g -111" \
	"backhaul multi-ap" "backhaul on"; do
	set -- $bad
	UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
	option band_steer_rssi_24g '-60'
	option band_steer_rssi_5g '-70'
	option roam_rssi_24g '-60'
	option roam_rssi_5g '-70'
	option $1 '$2'"
	case "$1" in band_steer_rssi_24g|roam_rssi_5g) ;; *) ;; esac
	run "bad-$1-$2" 1 "$1"
done
# a threshold of only one band
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
	option roam_rssi_24g '-60'"
run roam-one-band 1 "roam_rssi"

# 10. slots and ACL
GUEST="config zte_mesh_bss 'g24'
	option band '24g'
	option slot '1'
	option enabled '0'
	option ssid 'guest'
	option key 'guest-pass-1'
config zte_mesh_bss 'g5'
	option band '5g'
	option slot '1'
	option enabled '1'
	option ssid 'guest5'
	option key 'guest-pass-5'
	option hidden '1'
	option security 'psk-mixed'
	option acl_policy 'ban'
config zte_mesh_bss 'h5'
	option band '5g'
	option slot '2'
	option enabled '1'
	option ssid 'back'
	option key 'back-pass-1'"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
$GUEST
config zte_mesh_acl
	option mac 'AA:BB:CC:00:11:22'
	option name 'phone'
	option band '5g'
	option bss '1'
config zte_mesh_acl 'second'
	option mac '02:00:00:00:00:01'"
run slots-acl 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
radio0.bss1.enabled=0
radio0.bss1.ssid=guest
radio0.bss1.key=guest-pass-1
radio1.bss1.enabled=1
radio1.bss1.ssid=guest5
radio1.bss1.key=guest-pass-5
radio1.bss1.hidden=1
radio1.bss1.beacon_type=6
radio1.bss1.acl_policy=ban
radio1.bss2.enabled=1
radio1.bss2.ssid=back
radio1.bss2.key=back-pass-1
acl.1.mac=AA:BB:CC:00:11:22
acl.1.name=phone
acl.1.radio=1
acl.1.bss=1
acl.2.mac=02:00:00:00:00:01"

# gap, duplicate, incomplete slot, bad band/slot
SLOT3="config zte_mesh_bss 'x'
	option band '24g'
	option slot '3'
	option enabled '1'
	option ssid 'hid'
	option key 'hidden-key-1'"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
$SLOT3"
run slot-gap 1 "slot 3 needs slot 2"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
config zte_mesh_bss 'x'
	option band '24g'
	option slot '2'
	option enabled '1'
	option ssid 'two'
	option key 'second-key-1'"
run slot-two-without-one 1 "slot 2 needs slot 1"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
$GUEST
config zte_mesh_bss 'dup'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'dup'
	option key 'duplicate-1'"
run slot-duplicate 1 "configured twice"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
config zte_mesh_bss 'x'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'nokey'"
run slot-no-key 1 "required"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
config zte_mesh_bss 'x'
	option band '6g'
	option slot '1'"
run slot-bad-band 1 "band must be 24g or 5g"
UCI_TEXT="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'
config zte_mesh_bss 'x'
	option band '24g'
	option slot '4'"
run slot-bad-slot 1 "slot must be"
# ACL: multicast mac, zero mac, junk, name, band, bss without a slot, both bands needs both slots, too many rules
ACLBASE="$BASE
	option wifi_5g_ssid 'b'
	option wifi_5g_key 'password-5g'"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac '01:00:5e:00:00:01'"
run acl-multicast 1 "unicast"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac '00:00:00:00:00:00'"
run acl-zero 1 "unicast"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac 'aa:bb:cc:dd:ee'"
run acl-short 1 "unicast"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac 'aa:bb:cc:dd:ee:ff'
	option name 'bad name'"
run acl-name 1 "name may only"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac 'aa:bb:cc:dd:ee:ff'
	option band '6g'"
run acl-band 1 "band must be"
UCI_TEXT="$ACLBASE
config zte_mesh_acl
	option mac 'aa:bb:cc:dd:ee:ff'
	option bss '1'"
run acl-no-slot 1 "slot 1 is not configured"
UCI_TEXT="$ACLBASE
$GUEST
config zte_mesh_acl
	option mac 'aa:bb:cc:dd:ee:ff'
	option bss '2'"
run acl-both-needs-both 1 "slot 2 is not configured on both bands"
many="$ACLBASE"
i=1; while [ $i -le 17 ]; do many="$many
config zte_mesh_acl
	option mac '02:00:00:00:00:$(printf %02x $i)'"; i=$((i + 1)); done
UCI_TEXT="$many"
run acl-17 1 "more than 16"

# 10b. client actions (zte_mesh_client): ban and kick in file order, lower-cased, plus the one global ban time
UCI_TEXT="$ACLBASE
	option ban_minutes '45'
config zte_mesh_client
	option mac 'AA:BB:CC:DD:EE:01'
	option action 'ban'
config zte_mesh_client 'k'
	option mac 'aa:bb:cc:dd:ee:02'
	option action 'kick'
config zte_mesh_client 'd'
	option mac 'aa:bb:cc:dd:ee:03'"
run clients 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
ban.minutes=45
ban.mac=aa:bb:cc:dd:ee:01
kick.mac=aa:bb:cc:dd:ee:02
ban.mac=aa:bb:cc:dd:ee:03"
# No ban_minutes: the key is absent and the service's own default (until removed) applies.
UCI_TEXT="$ACLBASE
config zte_mesh_client
	option mac 'aa:bb:cc:dd:ee:01'"
run client-defaults 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
ban.mac=aa:bb:cc:dd:ee:01"
grep -q '^ban.minutes=' "$RUNDIR/config" && { echo "FAIL client-defaults: ban.minutes written without the option"; fail=$((fail + 1)); }
# Same device twice (the service keeps one row per address, so this would silently fight over it), bad action, bad MAC,
# a ban time outside the service's own 0..1440, and more rules than the table holds.
UCI_TEXT="$ACLBASE
config zte_mesh_client
	option mac 'aa:bb:cc:dd:ee:01'
config zte_mesh_client
	option mac 'AA:BB:CC:DD:EE:01'"
run client-duplicate 1 "is listed twice"
UCI_TEXT="$ACLBASE
config zte_mesh_client
	option mac 'aa:bb:cc:dd:ee:01'
	option action 'deauth'"
run client-action 1 "action must be ban or kick"
UCI_TEXT="$ACLBASE
config zte_mesh_client
	option mac '01:00:5e:00:00:01'"
run client-multicast 1 "unicast"
UCI_TEXT="$ACLBASE
	option ban_minutes '1441'
config zte_mesh_client
	option mac 'aa:bb:cc:dd:ee:01'"
run client-minutes 1 "ban_minutes must be"
many="$ACLBASE"
i=1; while [ $i -le 9 ]; do many="$many
config zte_mesh_client
	option mac '02:00:00:00:01:$(printf %02x $i)'"; i=$((i + 1)); done
UCI_TEXT="$many"
run client-9 1 "more than 8"

# 11. Wi-Fi schedule (zte_mesh_timer) and the extra slot options access_minutes / force_wifi5
STUB_TZ=+0800
UCI_TEXT="$ACLBASE
config zte_mesh_timer
	option start '23:00'
	option end '07:00'
	option days 'mon tue'
config zte_mesh_timer 'guest_night'
	option start '01:30'
	option end '05:45'
	option scope 'guest'"
run timers 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
timer.1.start=23:00
timer.1.end=07:00
timer.1.days=1100000
timer.1.scope=all
timer.2.start=01:30
timer.2.end=05:45
timer.2.days=1111111
timer.2.scope=guest
timer.utc_offset=480"
STUB_TZ=-0930
UCI_TEXT="$ACLBASE
config zte_mesh_timer
	option start '00:00'
	option end '00:01'"
run timer-negative-offset 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
timer.1.start=00:00
timer.1.end=00:01
timer.1.days=1111111
timer.1.scope=all
timer.utc_offset=-570"
STUB_TZ=+0545
run timer-nepal-offset 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
timer.1.start=00:00
timer.1.end=00:01
timer.1.days=1111111
timer.1.scope=all
timer.utc_offset=345"
STUB_TZ=+0000
for bad in "start 24:00" "start 7:00" "start 23:60" "start 23-00" "end 07:00" "days 'mon xyz'" "days ''" "scope night"; do
	set -- $bad
	k=$1; shift
	v="$*"; v=${v#\'}; v=${v%\'}
	UCI_TEXT="$ACLBASE
config zte_mesh_timer
	option start '23:00'
	option end '07:00'
	option $k '$v'"
	case "$k" in end) UCI_TEXT="$ACLBASE
config zte_mesh_timer
	option start '07:00'
	option end '07:00'" ;; esac
	run "bad-timer-$k-$v" 1 "zte_mesh_timer"
done
many="$ACLBASE"
i=1; while [ $i -le 9 ]; do many="$many
config zte_mesh_timer
	option start '0$i:00'
	option end '0$i:30'"; i=$((i + 1)); done
UCI_TEXT="$many"
run timer-9 1 "more than 8"
UCI_TEXT="$ACLBASE
config zte_mesh_bss 'g'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'guest'
	option key 'guest-pass-1'
	option access_minutes '120'
	option force_wifi5 'on'"
run slot-access-wifi5 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
radio1.ssid=b
radio1.key=password-5g
radio0.bss1.enabled=1
radio0.bss1.ssid=guest
radio0.bss1.key=guest-pass-1
radio0.bss1.access_minutes=120
radio0.bss1.force_wifi5=1"
for bad in "access_minutes 0" "access_minutes 10081" "access_minutes abc" "force_wifi5 2"; do
	set -- $bad
	UCI_TEXT="$ACLBASE
config zte_mesh_bss 'g'
	option band '24g'
	option slot '1'
	option enabled '1'
	option ssid 'guest'
	option key 'guest-pass-1'
	option $1 '$2'"
	run "bad-slot-$1-$2" 1 "$1"
done

# start_service() picks the emulator and this harness only runs write_config(), so guard the source
# instead. Without the ld-linux.so.3 probe, a board whose firmware does not ship /proc/config.gz
# (stock ImmortalWrt on an ax3600, for one) is read as "no CONFIG_COMPAT" and pushed onto an
# emulator that no package ships, and the sandbox then dies with a bare execv errno=8 -- which is
# exactly what a fresh install hit on 2026-10-31 before this existed.
for needle in 'CONFIG_COMPAT=y' 'ld-linux.so.3' 'sage:' 'qemu-arm-static' 'no qemu-arm-static was found'; do
	if grep -q -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL emulator source: $needle"
	fi
done

# start_service() also owns the controller address (only the last byte is configurable, and the value is
# handed to both the emulated daemon and mesh-search), so guard the source the same way.
for needle in 'config_get al_last_override main al_last' '02:00:00:00:19:$al_last' 'ZTE_MESH_AL_LAST=$al_last' 'is not two hex digits'; do
	if grep -q -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL al_last source: $needle"
	fi
done

# Every instance in start_service() is either long-lived or restarts by design, so none may sit on
# procd's default respawn budget: a bare `respawn` is filled in as 3600 5 5, and mesh-search exits
# every 300 s by design, which spends five retries in half an hour and drops the instance for good.
# Measured on the lab on 2026-11-02 -- search was already dead 30 minutes into the run, and a child
# that booted after that was never found. retry 0 is unlimited (also measured: 13 exits in 50 s,
# still restarting).
if [ "$(grep -c 'procd_set_param respawn' "$INIT")" -eq \
	"$(grep -c 'procd_set_param respawn 3600 5 0' "$INIT")" ]; then
	pass=$((pass + 1))
else
	fail=$((fail + 1)); echo "FAIL respawn budget: an instance still runs on procd's defaults"
fi

# A veth that is gone cannot be rebuilt from inside the sandbox -- no /proc, no /sys, no ip, no
# CAP_NET_ADMIN, and the daemon keeps the ifindex it bound to -- so attach is the only place that can
# notice "controller alive, wire missing" and restart the instance that owns the namespace.
ATTACH_SRC=$(dirname "$INIT")/attach
for needle in 'sandbox_pid' 'GRACE=' 'restarting it to rebuild the wire'; do
	if grep -q -- "$needle" "$ATTACH_SRC"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL wire healing: $needle"
	fi
done

# `backhaul` only ever describes a radio link on this router: a client to the controller's backhaul BSS on a
# child, that BSS itself on a broadcasting master. A controller-only router starts no wlan-sync instance at all,
# so the option does nothing there -- and it is usually left on by a box that changed role. start_service() says
# so once, because the page shows the option enabled and nothing on the air.
for needle in 'config_get backhaul main backhaul off' \
	'backhaul=multi_ap has nothing to do while apply_local is off' \
	'or mesh_role=slave to join the controller'; do
	if grep -q -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL backhaul warning: $needle"
	fi
done

# One judgement decides two things: whether a wlan-sync instance runs, and whether the restart signature carries the
# backhaul mode. If the two ever disagree, a save either restarts a box that applies nothing at all or leaves an
# applier running on a mode it will never be shown -- and the instance line lives in start_service(), which this
# harness cannot run, so the source is pinned to the shared helper instead.
for needle in 'sync_runs()' '&& sync_runs "$role" "$apply_local"' \
	'echo "backhaul=$backhaul extra_slots=$EXTRA_SLOTS pin2g=$pin2g pin5g=$pin5g"'; do
	if grep -q -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL applier judgement: $needle"
	fi
done

# The station counts an OpenWrt child answers an AP Metric with are measured on the host -- the radios are in /sys and
# ns-run keeps /sys out of the private root -- so a loop writes them to $RUNDIR/ap-sta for the responder inside. Only
# a child runs it (the controller is the one asking), and a stopped service must not leave a table beside a daemon
# that no longer reads it. start_service() again lives outside what this harness can run, so the judgement is pinned
# in the source.
for needle in 'STA_COLLECT=${ZTE_MESH_STA_COLLECT:-/usr/libexec/zte-mesh/sta-collect}' \
	'if ! role_is_master "$role" && [ -x "$STA_COLLECT" ]; then' \
	'procd_open_instance sta-collect' \
	'procd_set_param command "$STA_COLLECT" "$RUNDIR/ap-sta"' \
	'"$RUNDIR/wlan-plan" "$RUNDIR/ap-sta"'; do
	if grep -q -F -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL station count collection: $needle"
	fi
done

# The launcher is arch:all and lives in the same apk as the runtime now -- one build per CPU under
# arch/<cpu>/, and a shell entry that picks the directory matching `uname -m`. Nothing at install time
# refuses a machine it cannot run any more, so start_service() has to ask before procd gets an instance: a
# command that exits 127 is respawned every five seconds with nothing in the log about the architecture.
# The exit code is the whole judgement, so the needles are the question, the two answers and the fact that
# they are keyed off the number -- an ELF left under this name by hand answers 64, and calling that "no
# build for this machine" is a diagnosis that sends the reader to the wrong file (read back in a real
# upgrade on 2026-11-05; docs/journal/2026-11-05.md).
for needle in '"$NSRUN" --launcher-check 2>&1' 'launcher_status=$?' '"$launcher_status" = 127' \
	'has no launcher build for' '--launcher-check exited $launcher_status' \
	'$NSRUN is missing from package zte-mesh'; do
	if grep -q -F -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL launcher pre-flight: $needle"
	fi
done

# ... and the arch sentence must be the one gated by 127, not a bare `|| {`: any non-zero used to reach it.
if grep -F -- '--launcher-check 2>&1' "$INIT" | grep -q '|| {$'; then
	fail=$((fail + 1)); echo "FAIL launcher pre-flight: every non-zero still reads as an architecture refusal"
else
	pass=$((pass + 1))
fi

# The package this file used to tell people to install does not exist any more. A leftover mention is not
# cosmetic: it sends the reader to an `apk add` that the solver cannot satisfy.
if grep -q 'zte-mesh-launcher' "$INIT"; then
	fail=$((fail + 1)); echo "FAIL launcher pre-flight: $INIT still names the package zte-mesh-launcher"
else
	pass=$((pass + 1))
fi

# The two halves of that question are on both sides of one exec, so they can drift apart without either file
# looking wrong: the flag zte-mesh.init calls with is the one the entry script answers with.
DISPATCH_SRC=$(dirname "$INIT")/launcher-dispatch
if [ -f "$DISPATCH_SRC" ] && grep -q -F -- '--launcher-check' "$INIT" && grep -q -F -- '--launcher-check' "$DISPATCH_SRC"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1)); echo "FAIL launcher pre-flight: --launcher-check is not the same word on both sides"
fi

# One package, one install rule: the entry script and the per-CPU builds it looks for have to be installed by
# the same Makefile that this init script ships in, or a box ends up with an $NSRUN and no arch/ under it.
MK=$(dirname "$INIT")/../Makefile
if grep -qF -- 'files/launcher-dispatch' "$MK" && grep -qF -- '$(PKG_BUILD_DIR)/arch' "$MK"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1)); echo "FAIL launcher pre-flight: $MK installs neither the entry script nor arch/"
fi

# The three refusals above are three different sentences, and the greps cannot see the order they happen in:
# move the `[ -x "$NSRUN" ]` line below the check and every needle still matches, while the machine that lost
# its entry file goes back to being told its CPU is not in the package (read back on owrt, where uninstalling
# the old launcher package took both names away and the log said `no launcher build for x86_64`). So run the
# pre-flight and read the log line it actually wrote -- launcher_ready() is split out of start_service() for
# exactly this, because the rest of that function wants /sys/class/net and procd.
preflight() { # NAME WANT FORBID STUB-EXIT STUB-MSG
	local name="$1" want="$2" forbid="$3" code="$4" msg="$5" rc got
	rm -rf "$T/pf" >/dev/null 2>&1
	mkdir -p "$T/pf/root" || { fail=$((fail + 1)); echo "FAIL $name: no scratch tree"; return; }
	if [ -n "$code" ]; then
		printf '#!/bin/sh\nprintf %s >&2\nexit %s\n' "'$msg'" "$code" >"$T/pf/ns-run"
		chmod 755 "$T/pf/ns-run"
	fi
	rm -f "$LOGF"
	: >"$LOGF"
	ROOT=$T/pf/root NSRUN=$T/pf/ns-run
	launcher_ready; rc=$?
	got=$(cat "$LOGF")
	if [ "$rc" != 1 ]; then
		fail=$((fail + 1)); echo "FAIL $name: launcher_ready rc=$rc want 1"
	elif [ "$got" != "$want" ]; then
		fail=$((fail + 1)); echo "FAIL $name: log is '$got', want '$want'"
	else
		pass=$((pass + 1))
	fi
	[ "$rc" = 1 ] && [ "$got" = "$want" ] || return 0
	case $got in
	*"$forbid"*) fail=$((fail + 1)); echo "FAIL $name: this line also says '$forbid' -- two refusals in one" ;;
	*) pass=$((pass + 1)) ;;
	esac
}
# 1. the entry script is not on the box at all: that is a package that lost a file, and the only repair is
#    to reinstall -- saying "your CPU is not carried" would send the reader to a different architecture.
#    This is also the case that fails if the guard moves below the check: ash answers a missing file with 127.
preflight 'entry missing -> names the file' \
	"$T/pf/ns-run is missing from package zte-mesh" 'no launcher build for' '' ''
# 2. the entry script is there and answers 127: now it *is* the architecture answer, and the line carries the
#    machine it is standing on -- with the entry script's own words after it, not a summary of them.
preflight 'arch refused -> names the CPU' \
	"zte-mesh has no launcher build for $(uname -m): zte-mesh ns-run: no build for this machine" \
	'is missing from package' 127 'zte-mesh ns-run: no build for this machine'
# 3. a non-zero that is not 127 is some other problem wearing the same exit -- an ELF somebody left under
#    this name by hand takes --launcher-check as unknown arguments and exits 64.
preflight 'usage exit -> names the code' \
	"$T/pf/ns-run --launcher-check exited 64: usage: ns-run" \
	'no launcher build for' 64 'usage: ns-run'
ROOT=/usr/lib/zte-mesh/root NSRUN=/usr/libexec/zte-mesh/ns-run

# ... and the pre-flight has to actually be on the path start_service() takes: a function nobody calls would
# keep every judgement above green while procd respawned a 127 every five seconds again.
for needle in 'launcher_ready || return 1' 'launcher_ready() {'; do
	if grep -qF -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL launcher pre-flight: $INIT has no '$needle'"
	fi
done

# zte_mesh.main.emulator accepts a path on the host as well as one inside the private root. ns-run execs the
# emulator after the chroot, so a path the sandbox cannot see is not a path it can run: the copy is the
# whole feature, the hash comparison is what stops a restart from rewriting an 11 MB file every time the
# service starts, and a copy that fails must not leave an instance pointing at a file that is not there.
for needle in 'host_bin=$emulator' 'sha256sum "$host_bin"' 'copied $host_bin into the private root' \
	'is not readable: not starting' 'cannot copy $host_bin into $dest'; do
	if grep -q -F -- "$needle" "$INIT"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); echo "FAIL emulator from the host: $needle"
	fi
done

echo "init test: $pass passed, $fail failed"
[ "$fail" = 0 ]
