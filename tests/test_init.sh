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
pass=0; fail=0; AFTER_LOAD=''

logger() { for _a; do :; done; echo "$_a" >> "$LOGF"; }
date() { printf '%s\n' "${STUB_TZ:-+0000}"; }
config_load() {
	SECS=""; local line cur="" n=0 k v t
	while IFS= read -r line; do
		line=${line#"${line%%[![:space:]]*}"}
		case "$line" in
		\#*) ;;
		config\ *)
			set -- $line; t=$2; cur=${3:-}; cur=${cur#\'}; cur=${cur%\'}
			[ -n "$cur" ] || { n=$((n + 1)); cur=cfg$n; }
			SECS="$SECS $cur"; eval "T_$cur=\$t" ;;
		option\ *)
			line=${line#option }; k=${line%% *}; v=${line#* }
			# like libuci: a quoted value ends at its closing quote, an unquoted '#' starts a comment
			case "$v" in \'*) v=${v#\'}; v=${v%%\'*} ;; *) v=${v%%[[:space:]#]*} ;; esac
			eval "V_${cur}_$k=\$v" ;;
		esac
	done < "$UCI"
	[ -z "$AFTER_LOAD" ] || eval "$AFTER_LOAD"
}
config_get() {
	local _s
	eval "_s=\${V_${2}_${3}+set}"
	if [ -n "$_s" ]; then eval "$1=\$V_${2}_${3}"; else eval "$1=\${4:-}"; fi
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

# uci TEXT ... ; case NAME EXPECT-RC [expected output file text | -]
run() {
	local name="$1" want_rc="$2" expect="$3" rc got
	rm -rf "$RUNDIR" "$LOGF"; : > "$LOGF"
	UCI=$T/uci; printf '%s\n' "$UCI_TEXT" > "$UCI"
	for _v in $(set | grep -o '^[VT]_[A-Za-z0-9_]*=' | tr -d '='); do unset "$_v"; done
	write_config; rc=$?
	if [ "$rc" != "$want_rc" ]; then
		echo "FAIL $name: rc=$rc want $want_rc ($(tail -1 "$LOGF"))"; fail=$((fail + 1)); return
	fi
	if [ "$want_rc" = 0 ]; then
		got=$(cat "$RUNDIR/config")
		[ -z "$(find "$RUNDIR/config" -perm -077 2>/dev/null)" ] || { echo "FAIL $name: config is not 0600"; fail=$((fail + 1)); return; }
		[ "$got" = "$expect" ] || { echo "FAIL $name: output differs"; echo "--- got"; echo "$got"; echo "--- want"; echo "$expect"; fail=$((fail + 1)); return; }
		ls "$RUNDIR"/.config.* >/dev/null 2>&1 && { echo "FAIL $name: temp file left behind"; fail=$((fail + 1)); return; }
	else
		[ ! -e "$RUNDIR/config" ] || { echo "FAIL $name: a refused configuration still wrote the file"; fail=$((fail + 1)); return; }
		ls "$RUNDIR"/.config.* >/dev/null 2>&1 && { echo "FAIL $name: temp file left behind"; fail=$((fail + 1)); return; }
		case "$(cat "$LOGF")" in *"$expect"*) ;; *) echo "FAIL $name: log lacks '$expect': $(tail -1 "$LOGF")"; fail=$((fail + 1)); return ;; esac
	fi
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
[ "$(cat "$RUNDIR/restart.sig")" = "128 " ] || { echo "FAIL legacy: restart.sig='$(cat "$RUNDIR/restart.sig")'"; fail=$((fail + 1)); }

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
	option mesh_v2 '1'
	option mesh_role 'auto'
	option mlo_enable '0'
	option band_steer '1'
	option band_steer_rssi_24g '-60'
	option band_steer_rssi_5g '-110'
	option roam_rssi_24g '-60'
	option roam_rssi_5g '-70'
	option roam_force '0'"
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
mesh.role=auto
mlo.enable=0
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70"
[ "$(cat "$RUNDIR/restart.sig")" = "band_steer.enabled band_steer.rssi24 band_steer.rssi5 mesh.role mlo.enable roam.force roam.rssi24 roam.rssi5 wifi.dual_band_sync 128 " ] ||
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
mesh.role=auto
mlo.enable=0
band_steer.enabled=1
roam.force=0
band_steer.rssi24=-60
band_steer.rssi5=-110
roam.rssi24=-60
roam.rssi5=-70"

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

# 8g. main router: the controller that also broadcasts. The daemon hears "auto" -- that is what this role was called
# before the three-way split, so its behaviour is unchanged -- and the host side gets a plan for its own radios, which
# a controller-only router never produces. The 5 GHz row spells out what the daemon derives for itself.
UCI_TEXT="$BASE
	option mesh_role 'main'"
run role-main 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=auto"
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = "# zte-mesh local Wi-Fi plan (main router). Generated by the service; applied by wlan-sync.
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

# 8h. main router with synchronised bands: the 5 GHz row is the 2.4 GHz row, hidden flag and security included, and an
# empty 5 GHz pair must not leak into it
UCI_TEXT="$BASE
	option wifi_5g_ssid 'ignored'
	option wifi_5g_key 'ignored-key'
	option wifi_dual_band_sync '1'
	option wifi_24g_hidden '1'
	option wifi_24g_security 'sae'
	option mesh_role 'main'"
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
mesh.role=auto"
[ "$(cat "$RUNDIR/wlan-plan" 2>/dev/null)" = "# zte-mesh local Wi-Fi plan (main router). Generated by the service; applied by wlan-sync.
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

# 8i. controller only: the profile is handed out and this router's radios are left alone, so no plan -- and a plan
# left behind by an earlier role is removed instead of being applied by a wlan-sync that no longer runs
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

# 8j. "auto" is the old name of the main router role: same plan, same wire value, so an upgraded configuration keeps
# working without the user touching the form
UCI_TEXT="$BASE
	option mesh_role 'auto'"
run role-auto-legacy 0 "version_mode=128
radio0.enabled=1
radio0.ssid=home
radio0.key=password-24
radio1.enabled=1
mesh.role=auto"
grep -q '^1.ssid=home-5G$' "$RUNDIR/wlan-plan" 2>/dev/null ||
	{ echo "FAIL role-auto-legacy: no plan for the legacy name of the main router role"; fail=$((fail + 1)); }

# 8d. child role with nothing configured: the profile it starts from is the network this router already broadcasts,
# so setting up a child is one option (mesh_role=slave) and not a form to fill in
WIRELESS_DB=$T/wireless
printf '%s\n' 'wireless.default_radio0=wifi-iface' 'wireless.default_radio0.ssid=ChildAP' \
	'wireless.default_radio0.key=childpass' > "$WIRELESS_DB"
UCI_TEXT="config zte_mesh 'main'
	option wifi_24g_enabled '1'
	option wifi_5g_enabled '1'
	option mesh_role 'slave'"
run role-slave-borrow 0 "version_mode=128
radio0.enabled=1
radio0.ssid=ChildAP
radio0.key=childpass
radio1.enabled=1
mesh.role=slave"
grep -q 'starting from the network this router broadcasts' "$LOGF" ||
	{ echo "FAIL role-slave-borrow: the borrowed profile is not logged"; fail=$((fail + 1)); }

# 8e. child role, no profile and nothing to borrow: refuses, and the message says what a child actually needs
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
	"wifi_5g_channel 37" "wifi_24g_tx_power 31" "wifi_24g_tx_power -1" "wifi_24g_acl_policy deny" "mesh_v2 2" "mesh_role child" \
	"mlo_enable x" "band_steer maybe" "roam_force 5" "wifi_dual_band_sync 2" "band_steer_rssi_24g -29" "roam_rssi_5g -111"; do
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

echo "init test: $pass passed, $fail failed"
[ "$fail" = 0 ]
