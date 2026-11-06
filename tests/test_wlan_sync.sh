#!/bin/sh
# Host test of openwrt/zte-mesh/files/wlan-sync: the plan the service publishes in the child (agent) role is
# mapped onto /etc/config/wireless. The UCI command line is replaced by a tiny key/value store with the same
# semantics for show/get/set/delete/commit, so the test runs anywhere.
#
#   sh tests/test_wlan_sync.sh [WLAN_SYNC]
WLAN_SYNC=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/wlan-sync}
T=$(mktemp -d) || exit 1
# Git Bash's mktemp hands back a Windows drive prefix (C:\...), which the delete helpers on that host
# refuse; going through pwd gives the POSIX form there and changes nothing on Linux.
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
DB=$T/db
mkdir -p "$DB" "$T/bin"
LOG=$DB.log

cat > "$T/bin/uci" <<'UCI'
#!/bin/sh
DB=${UCI_DB:?}
quiet=0
[ "${1:-}" = -q ] && { quiet=1; shift; }
cmd=${1:-}; shift 2>/dev/null || true
missing() { [ "$quiet" = 1 ] || echo "uci: Entry not found" >&2; exit 1; }
case "$cmd" in
show)
	for f in "$DB"/type.*; do
		[ -e "$f" ] || continue
		s=${f##*/type.}
		echo "wireless.$s=$(cat "$f")"
	done
	;;
get)
	# wireless.<sec>.<opt> | wireless.<sec> (its type) | <config>.<sec>.<opt> stored under its own name
	case "$1" in
	wireless.*)
		rest=${1#wireless.}
		case "$rest" in
		*.*) s=${rest%%.*}; o=${rest#*.}
			[ -f "$DB/$s.$o" ] && { cat "$DB/$s.$o"; exit 0; }
			missing ;;
		*)  [ -f "$DB/type.$rest" ] && { cat "$DB/type.$rest"; exit 0; }
			missing ;;
		esac
		;;
	*)
		[ -f "$DB/$1" ] && { cat "$DB/$1"; exit 0; }
		missing ;;
	esac
	;;
set)
	case "$1" in
	wireless.*)
		rest=${1#wireless.}
		case "$rest" in
		*.*) s=${rest%%.*}; rest=${rest#*.}; o=${rest%%=*}; v=${rest#*=}
			printf '%s' "$v" > "$DB/$s.$o"
			echo "set $s.$o=$v" >> "$UCI_DB.log" ;;
		*)  s=${rest%%=*}; v=${rest#*=}
			printf '%s' "$v" > "$DB/type.$s"
			echo "set $s=$v" >> "$UCI_DB.log" ;;
		esac
		;;
	*) name=${1%%=*}; v=${1#*=}
		printf '%s' "$v" > "$DB/$name"
		echo "set $name=$v" >> "$UCI_DB.log" ;;
	esac
	;;
delete)
	case "$1" in
	wireless.*)
		rest=${1#wireless.}
		case "$rest" in
		*.*) s=${rest%%.*}; o=${rest#*.}
			rm -f "$DB/$s.$o"
			echo "delete $s.$o" >> "$UCI_DB.log" ;;
		*)  rm -f "$DB/type.$rest" "$DB/$rest".*
			echo "delete $rest" >> "$UCI_DB.log" ;;
		esac
		;;
	*) rm -f "$DB/$1" ;;
	esac
	;;
commit)
	echo "commit $*" >> "$UCI_DB.log"
	;;
*)
	[ "$quiet" = 1 ] || echo "uci: unknown $cmd" >&2
	exit 1
	;;
esac
UCI
cat > "$T/bin/wifi" <<'WIFI'
#!/bin/sh
echo "wifi $*" >> "$UCI_DB.log"
WIFI
cat > "$T/bin/logger" <<'LOGGER'
#!/bin/sh
shift 2 2>/dev/null
echo "log $*" >> "$UCI_DB.log"
LOGGER
chmod +x "$T/bin/uci" "$T/bin/wifi" "$T/bin/logger"
export UCI_DB=$DB
PATH=$T/bin:$PATH
export PATH

# A router as OpenWrt describes it: two radios, one AP interface each. Re-seeded before every case so one
# case cannot pass on another's leftovers.
seed_db() {
	rm -f "$DB"/* "$LOG"
	printf 'wifi-device' > "$DB/type.radio0"; printf '2g' > "$DB/radio0.band"
	printf 'wifi-device' > "$DB/type.radio1"; printf '5g' > "$DB/radio1.band"
	printf 'wifi-iface' > "$DB/type.default_radio0"
	printf 'radio0' > "$DB/default_radio0.device"; printf 'ap' > "$DB/default_radio0.mode"
	printf 'old-2g' > "$DB/default_radio0.ssid"; printf 'old-key-2g' > "$DB/default_radio0.key"
	printf 'psk2' > "$DB/default_radio0.encryption"; printf '0' > "$DB/default_radio0.disabled"
	printf 'wifi-iface' > "$DB/type.default_radio1"
	printf 'radio1' > "$DB/default_radio1.device"; printf 'ap' > "$DB/default_radio1.mode"
	printf 'old-5g' > "$DB/default_radio1.ssid"; printf 'old-key-5g' > "$DB/default_radio1.key"
	printf 'psk2' > "$DB/default_radio1.encryption"; printf '0' > "$DB/default_radio1.disabled"
	# Both main BSSs sit on the LAN, which is the network an extra slot inherits (there is no VLAN separation).
	printf 'lan' > "$DB/default_radio0.network"; printf 'lan' > "$DB/default_radio1.network"
	: > "$LOG"
}
seed_db

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}
val() { cat "$DB/$1" 2>/dev/null; }
# The band lookup runs once per plan row and once more from clear_backhaul, so a message about a band can appear
# more than once in a run: these cases ask whether it was said, not how many times.
has() { [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge 1 ] && echo yes || echo no; }
# Every run leaves a status file next to the plan (wlan-sync.status): the sandboxed service and the page cannot see
# what happens outside the sandbox, and without it a failed apply is only a line in the system log.
status_val() { sed -n "s/^$1=//p" "$T/wlan-sync.status" 2>/dev/null | head -n 1; }

PLAN=$T/plan
cat > "$PLAN" <<'PLAN'
# zte-mesh agent synchronisation plan.
rows=2
0.radio=0
0.slot=0
0.instance=DEV.WIFI.AP1
0.psk_instance=DEV.WIFI.AP1.PSK1
0.ssid=zte-agent-2g
0.key=m2-public-2g-key
0.enabled=1
0.hidden=0
0.access=0
0.beacon_type=6
1.radio=1
1.slot=0
1.instance=DEV.WIFI.AP5
1.psk_instance=DEV.WIFI.AP5.PSK1
1.ssid=zte-agent-5g
1.key=m2-public-5g-key
1.enabled=1
1.hidden=1
1.access=0
1.beacon_type=3
PLAN

seed_db
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "2.4G name" zte-agent-2g "$(val default_radio0.ssid)"
check "2.4G key" m2-public-2g-key "$(val default_radio0.key)"
check "2.4G security (beacon 6 = WPA/WPA2 mixed)" psk-mixed "$(val default_radio0.encryption)"
check "2.4G enabled" 0 "$(val default_radio0.disabled)"
check "5G name" zte-agent-5g "$(val default_radio1.ssid)"
check "5G key" m2-public-5g-key "$(val default_radio1.key)"
check "5G security (beacon 3 = WPA2)" psk2 "$(val default_radio1.encryption)"
check "5G hidden" 1 "$(val default_radio1.hidden)"
check "one commit" 1 "$(grep -c '^commit wireless$' "$LOG")"
check "one reload" 1 "$(grep -c '^wifi reload$' "$LOG")"

# Idempotent: the same plan again writes, commits and reloads nothing.
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "second run sets nothing" 0 "$(grep -c '^set ' "$LOG")"
check "second run commits nothing" 0 "$(grep -c '^commit ' "$LOG")"
check "second run reloads nothing" 0 "$(grep -c '^wifi ' "$LOG")"

# A disabled row disables the radio's AP, an open row drops the key.
sed -e 's/^0.enabled=1$/0.enabled=0/' -e 's/^1.beacon_type=3$/1.beacon_type=0/' "$PLAN" > "$T/plan2"
seed_db
sh "$WLAN_SYNC" "$T/plan2" --once >"$T/out" 2>&1
check "disabled band" 1 "$(val default_radio0.disabled)"
check "open network security" none "$(val default_radio1.encryption)"
check "open network key removed" "" "$(val default_radio1.key)"

# An extra SSID slot is reported and skipped: the factory daemon owns those.
sed -e 's/^1.slot=0$/1.slot=2/' "$PLAN" > "$T/plan3"
seed_db
sh "$WLAN_SYNC" "$T/plan3" --once >"$T/out" 2>&1
check "slot 1 (5G) still applied" psk2 "$(val default_radio1.encryption)"
check "extra slot logged" 1 "$(grep -c 'slot 2 of radio 1 is not applied' "$LOG")"

# An unknown security mode leaves the interface alone instead of guessing.
sed -e 's/^1.beacon_type=3$/1.beacon_type=7/' "$PLAN" > "$T/plan4"
seed_db
sh "$WLAN_SYNC" "$T/plan4" --once >"$T/out" 2>&1
check "unknown mode leaves security" psk2 "$(val default_radio1.encryption)"
check "unknown mode logged" 1 "$(grep -c 'unknown security mode 7' "$LOG")"

# A missing plan is not an error: the loop waits for the service to publish one.
seed_db
sh "$WLAN_SYNC" "$T/none" --once >"$T/out" 2>&1
check "missing plan touches nothing" 0 "$(grep -c '^set ' "$LOG")"

# --- two devices on one band ------------------------------------------------------------------------------
# The band is a name, not a section. The xiaomi,ax3600 carries an unused PCIe 5 GHz card (radio0) next to the
# SoC's 5 GHz radio (radio1) and both declare band '5g'; matching the first device of the band wrote the
# controller's network onto the card, so the radio on the air kept its old SSID. Reproduced here, then pinned.
seed_db_two_5g() {
	rm -f "$DB"/* "$LOG"
	printf 'wifi-device' > "$DB/type.radio0"; printf '5g' > "$DB/radio0.band"; printf '1' > "$DB/radio0.disabled"
	printf 'wifi-iface' > "$DB/type.default_radio0"
	printf 'radio0' > "$DB/default_radio0.device"; printf 'ap' > "$DB/default_radio0.mode"
	printf 'old-card-5g' > "$DB/default_radio0.ssid"; printf 'old-key-card' > "$DB/default_radio0.key"
	printf 'psk2' > "$DB/default_radio0.encryption"; printf '0' > "$DB/default_radio0.disabled"
	printf 'wifi-device' > "$DB/type.radio1"; printf '5g' > "$DB/radio1.band"; printf '1' > "$DB/radio1.disabled"
	printf 'wifi-iface' > "$DB/type.default_radio1"
	printf 'radio1' > "$DB/default_radio1.device"; printf 'ap' > "$DB/default_radio1.mode"
	printf 'old-soc-5g' > "$DB/default_radio1.ssid"; printf 'old-key-soc' > "$DB/default_radio1.key"
	printf 'psk2' > "$DB/default_radio1.encryption"; printf '0' > "$DB/default_radio1.disabled"
	printf 'wifi-device' > "$DB/type.radio2"; printf '2g' > "$DB/radio2.band"
	printf 'wifi-iface' > "$DB/type.default_radio2"
	printf 'radio2' > "$DB/default_radio2.device"; printf 'ap' > "$DB/default_radio2.mode"
	printf 'old-2g' > "$DB/default_radio2.ssid"; printf 'old-key-2g' > "$DB/default_radio2.key"
	printf 'psk2' > "$DB/default_radio2.encryption"; printf '0' > "$DB/default_radio2.disabled"
	: > "$LOG"
}

# Two live candidates: the band is left alone and the log names the option that settles it.
seed_db_two_5g
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "two 5g radios: 2.4G still applied" zte-agent-2g "$(val default_radio2.ssid)"
check "two 5g radios: the card is not taken over" old-card-5g "$(val default_radio0.ssid)"
check "two 5g radios: the radio is not guessed either" old-soc-5g "$(val default_radio1.ssid)"
check "two 5g radios: ambiguity reported" yes "$(has 'several 5g radios')"
check "two 5g radios: the log names the option" yes "$(has 'zte_mesh.main.wifi_5g')"

# The pin settles it, and only the pinned device is written.
seed_db_two_5g
printf 'radio1' > "$DB/zte_mesh.main.wifi_5g"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "pin: 5G applied to the pinned radio" zte-agent-5g "$(val default_radio1.ssid)"
check "pin: the 5G key follows" m2-public-5g-key "$(val default_radio1.key)"
check "pin: the other 5G device untouched" old-card-5g "$(val default_radio0.ssid)"
check "pin: 2.4G applied as well" zte-agent-2g "$(val default_radio2.ssid)"

# A pin that names a device of the other band is reported and not obeyed.
seed_db_two_5g
printf 'radio2' > "$DB/zte_mesh.main.wifi_5g"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "bad pin: named in the log" yes "$(has "names 'radio2', which is not a 5g device")"
check "bad pin: 5G not written by mistake" old-soc-5g "$(val default_radio1.ssid)"
check "bad pin: the 2.4G radio kept its own name" zte-agent-2g "$(val default_radio2.ssid)"

# One of the two switched off is not ambiguous: that is the usual desk layout, and it needs no pin.
seed_db_two_5g
rm -f "$DB/radio1.disabled"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "one live 5g radio: applied there" zte-agent-5g "$(val default_radio1.ssid)"
check "one live 5g radio: no warning" 0 "$(grep -c 'several 5g radios' "$LOG")"

# --- radios that are switched off --------------------------------------------------------------------------
# The plan is the controller's word on what should be on the air, and a disabled wifi-device cannot put anything
# there. The factory image and the lab's ax3600 ship every device disabled, which is exactly how a child ends up
# with the controller's SSID in /etc/config/wireless and no BSS in the air.
seed_db
printf '1' > "$DB/radio1.disabled"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "radio off: switched on for the plan" 0 "$(val radio1.disabled)"
check "radio off: said so in the log" yes "$(has 'radio1 (5g) was switched off')"
check "radio off: that band's BSS is enabled" 0 "$(val default_radio1.disabled)"
check "radio off: nothing is invented for the other band" "" "$(val radio0.disabled)"

# A band the plan disables keeps its device: switching the device off would take the band's backhaul BSS with it.
sed -e 's/^0.enabled=1$/0.enabled=0/' "$PLAN" > "$T/plan-band-off"
seed_db
printf '1' > "$DB/radio0.disabled"
sh "$WLAN_SYNC" "$T/plan-band-off" --once >"$T/out" 2>&1
check "band off: the radio is left off" 1 "$(val radio0.disabled)"
check "band off: its BSS is switched off" 1 "$(val default_radio0.disabled)"

# A radio that is already on is not written: the loop runs every couple of seconds, it must not churn UCI.
seed_db
printf '0' > "$DB/radio1.disabled"
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "radio on: never written" 0 "$(grep -c '^set radio1.disabled' "$LOG")"

# UCI booleans have word forms, and a hand-edited config uses them.
seed_db
printf 'true' > "$DB/radio1.disabled"
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "radio off spelled true: switched on" 0 "$(val radio1.disabled)"

# --- a child's seed that equals the air ---------------------------------------------------------------------
# zte-mesh.init seeds a child from the network this router is already broadcasting, band by band, and its
# `live_ap_iface` resolves the same sections `radio_section_of` + `ap_iface_of` resolve here (tests/test_init.sh has
# the ax3600 fixture; the plan below is the service config it produced). The restart then has to find nothing to
# write: a single no-op write marks the plan applied, and an applied plan commits and reloads every radio on this
# router -- which is the client drop the two name flips used to cause.
PLAN_SEED=$T/plan-seed
cat > "$PLAN_SEED" <<'PLAN'
rows=2
0.radio=0
0.slot=0
0.instance=DEV.WIFI.AP1
0.ssid=FRESHR10
0.key=Ztemesh2631!
0.enabled=1
0.hidden=0
0.access=32
0.beacon_type=12
1.radio=1
1.slot=0
1.instance=DEV.WIFI.AP5
1.ssid=FRESHR10-5G
1.key=Ztemesh2631!
1.enabled=1
1.hidden=0
1.access=32
1.beacon_type=6
PLAN
seed_db_two_5g
rm -f "$DB/radio1.disabled"
printf 'FRESHR10' > "$DB/default_radio2.ssid"; printf 'Ztemesh2631!' > "$DB/default_radio2.key"
printf 'sae-mixed' > "$DB/default_radio2.encryption"; printf '0' > "$DB/default_radio2.hidden"
printf 'FRESHR10-5G' > "$DB/default_radio1.ssid"; printf 'Ztemesh2631!' > "$DB/default_radio1.key"
printf 'psk-mixed' > "$DB/default_radio1.encryption"; printf '0' > "$DB/default_radio1.hidden"
sh "$WLAN_SYNC" "$PLAN_SEED" --once >"$T/out" 2>&1
check "seed equals air: nothing written" 0 "$(grep -c '^set ' "$LOG")"
check "seed equals air: no commit" 0 "$(grep -c '^commit ' "$LOG")"
check "seed equals air: no reload" 0 "$(grep -c '^wifi ' "$LOG")"
check "seed equals air: reports no change" 0 "$(status_val changed)"
check "seed equals air: names no band" "" "$(status_val bands)"
check "seed equals air: still ok" ok "$(status_val result)"

# The same pair one letter away is still a change, so this cannot pass by never writing anything.
: > "$LOG"
sed -e 's/^0.ssid=FRESHR10$/0.ssid=FRESHR11/' "$PLAN_SEED" > "$T/plan-seed-diff"
sh "$WLAN_SYNC" "$T/plan-seed-diff" --once >"$T/out" 2>&1
check "seed differs: the name is written" FRESHR11 "$(val default_radio2.ssid)"
check "seed differs: one commit" 1 "$(grep -c '^commit wireless$' "$LOG")"

# A flag the config does not spell out already answers off, so the 0 a plan carries for it is not a difference:
# writing it would churn UCI and reload the radios for a network that did not move.
seed_db
rm -f "$DB/default_radio0.disabled" "$DB/default_radio1.disabled"
sed -e 's/^0.ssid=zte-agent-2g$/0.ssid=old-2g/' -e 's/^0.key=m2-public-2g-key$/0.key=old-key-2g/' \
	-e 's/^0.beacon_type=6$/0.beacon_type=3/' -e 's/^1.ssid=zte-agent-5g$/1.ssid=old-5g/' \
	-e 's/^1.key=m2-public-5g-key$/1.key=old-key-5g/' -e 's/^1.hidden=1$/1.hidden=0/' "$PLAN" > "$T/plan-absent-flags"
sh "$WLAN_SYNC" "$T/plan-absent-flags" --once >"$T/out" 2>&1
check "absent flags: nothing invented for disabled" "" "$(val default_radio0.disabled)"
check "absent flags: nothing invented for hidden" "" "$(val default_radio0.hidden)"
check "absent flags: no write" 0 "$(grep -c '^set ' "$LOG")"
check "absent flags: no reload" 0 "$(grep -c '^wifi ' "$LOG")"
# and a flag the plan *does* want on is still written
sed -e 's/^1.hidden=0$/1.hidden=1/' "$T/plan-absent-flags" > "$T/plan-hidden-on"
sh "$WLAN_SYNC" "$T/plan-hidden-on" --once >"$T/out" 2>&1
check "absent flags: a real 1 is written" 1 "$(val default_radio1.hidden)"

# --- backhaul (multi_ap) ---------------------------------------------------------------------------------
# A plan as the controller actually fills it: slot 0 is the fronthaul BSS (MapAccessAttr 32) and slot 3 of
# the 2.4 GHz band is the hidden backhaul BSS (64). Only the 2.4 GHz band gets a backhaul row here, so the
# 5 GHz half also proves that nothing is invented for a band the controller did not fill in.
backhaul_on() { printf 'multi_ap' > "$DB/zte_mesh.main.backhaul"; }
backhaul_off() { rm -f "$DB/zte_mesh.main.backhaul"; }

PLAN_BH=$T/plan-bh
cat > "$PLAN_BH" <<'PLAN'
rows=3
0.radio=0
0.slot=0
0.instance=DEV.WIFI.AP1
0.ssid=zte-agent-2g
0.key=m2-public-2g-key
0.enabled=1
0.hidden=0
0.access=32
0.beacon_type=6
1.radio=0
1.slot=3
1.instance=DEV.WIFI.AP4
1.ssid=ZMESH-bh-2g
1.key=backhaul-2g-key
1.enabled=1
1.hidden=1
1.access=64
1.beacon_type=6
2.radio=1
2.slot=0
2.instance=DEV.WIFI.AP5
2.ssid=zte-agent-5g
2.key=m2-public-5g-key
2.enabled=1
2.hidden=0
2.access=32
2.beacon_type=3
PLAN

# Off (the default) is byte-for-byte the old behaviour: the backhaul row is reported, nothing is created.
seed_db
backhaul_off
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "off: no backhaul section" "" "$(val type.zte_backhaul_2g)"
check "off: main BSS carries no multi_ap" "" "$(val default_radio0.multi_ap)"
check "off: backhaul row reported" 1 "$(grep -c 'backhaul mode is off; not applied' "$LOG")"
check "off: fronthaul row still applied" zte-agent-2g "$(val default_radio0.ssid)"

# On: the backhaul row becomes its own BSS, the main BSS of each band becomes the fronthaul half.
seed_db
backhaul_on
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "backhaul section type" wifi-iface "$(val type.zte_backhaul_2g)"
check "backhaul radio" radio0 "$(val zte_backhaul_2g.device)"
check "backhaul mode" ap "$(val zte_backhaul_2g.mode)"
check "backhaul multi_ap" 2 "$(val zte_backhaul_2g.multi_ap)"
check "backhaul name is the controller's" ZMESH-bh-2g "$(val zte_backhaul_2g.ssid)"
check "backhaul key is the controller's" backhaul-2g-key "$(val zte_backhaul_2g.key)"
check "backhaul security (beacon 6)" psk-mixed "$(val zte_backhaul_2g.encryption)"
check "backhaul hidden" 1 "$(val zte_backhaul_2g.hidden)"
check "backhaul enabled" 0 "$(val zte_backhaul_2g.disabled)"
check "fronthaul 2.4G marked multi_ap 1" 1 "$(val default_radio0.multi_ap)"
check "fronthaul 5G marked multi_ap 1" 1 "$(val default_radio1.multi_ap)"
check "no 5G backhaul row, no 5G section" "" "$(val type.zte_backhaul_5g)"
check "main SSID untouched by the backhaul row" zte-agent-2g "$(val default_radio0.ssid)"
check "one commit for the whole plan" 1 "$(grep -c '^commit wireless$' "$LOG")"

# Idempotent in this mode too.
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "second backhaul run sets nothing" 0 "$(grep -c '^set ' "$LOG")"
check "second backhaul run commits nothing" 0 "$(grep -c '^commit ' "$LOG")"
check "second backhaul run reloads nothing" 0 "$(grep -c '^wifi ' "$LOG")"

# The backhaul BSS needs a radio that is on as well, and that is the shape of a real child: its 2.4 GHz device is
# switched off, and the controller's backhaul rides that band. The fronthaul row comes first and switches it on.
seed_db
printf '1' > "$DB/radio0.disabled"
backhaul_on
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "backhaul on a switched-off radio: radio on" 0 "$(val radio0.disabled)"
check "backhaul on a switched-off radio: BSS created" wifi-iface "$(val type.zte_backhaul_2g)"

# A backhaul row the plan disables does not switch the device on only to switch its own BSS off again.
sed -e 's/^0.enabled=1$/0.enabled=0/' -e 's/^1.enabled=1$/1.enabled=0/' "$PLAN_BH" > "$T/plan-bh-off"
seed_db
printf '1' > "$DB/radio0.disabled"
backhaul_on
sh "$WLAN_SYNC" "$T/plan-bh-off" --once >"$T/out" 2>&1
check "disabled backhaul: the radio is left off" 1 "$(val radio0.disabled)"
check "disabled backhaul: BSS created but off" 1 "$(val zte_backhaul_2g.disabled)"

# Switching back to off undoes both halves, and says so.
backhaul_off
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "off again: section removed" "" "$(val type.zte_backhaul_2g)"
check "off again: fronthaul flag cleared" "" "$(val default_radio0.multi_ap)"
check "off again: removal logged" 1 "$(grep -c 'removed zte_backhaul_2g' "$LOG")"
check "off again: flag clearing logged" 1 "$(grep -c 'cleared multi_ap on default_radio0' "$LOG")"

# A multi_ap value this script never writes is somebody else's: off mode leaves it alone.
seed_db
printf '3' > "$DB/default_radio0.multi_ap"
backhaul_off
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "off: a foreign multi_ap 3 is kept" 3 "$(val default_radio0.multi_ap)"

# An open backhaul drops the key instead of writing an empty one, and an unknown mode is not guessed.
sed -e 's/^1.beacon_type=6$/1.beacon_type=0/' "$PLAN_BH" > "$T/plan-bh2"
seed_db
backhaul_on
sh "$WLAN_SYNC" "$T/plan-bh2" --once >"$T/out" 2>&1
check "open backhaul security" none "$(val zte_backhaul_2g.encryption)"
check "open backhaul key removed" "" "$(val zte_backhaul_2g.key)"

sed -e 's/^1.beacon_type=6$/1.beacon_type=9/' "$PLAN_BH" > "$T/plan-bh3"
seed_db
backhaul_on
sh "$WLAN_SYNC" "$T/plan-bh3" --once >"$T/out" 2>&1
check "unknown backhaul mode creates nothing" "" "$(val type.zte_backhaul_2g)"
check "unknown backhaul mode logged" 1 "$(grep -c 'backhaul on 2g: unknown security mode 9' "$LOG")"

# A plan without the access field (an older service build) still marks the fronthaul but invents no backhaul.
seed_db
backhaul_on
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "old plan: fronthaul still marked" 1 "$(val default_radio0.multi_ap)"
check "old plan: no backhaul section invented" "" "$(val type.zte_backhaul_2g)"

# --- the child end of the same row (backhaul client) ------------------------------------------------------
# The row whose MapAccessAttr carries bit 6 means the opposite thing depending on which end this router is on.
# `mesh_role=slave` puts this router on the client side, so the row becomes a `mode=sta` interface, and two
# options carry the whole feature -- neither is obvious from the plan:
#   multi_ap 1   netifd copies it into the supplicant's multi_ap_backhaul_sta, which is what lets the association
#                land on a backhaul BSS; a plain STA is refused by one, and the controller's backhaul SSID is hidden
#   wds 1        this image sets 4addr from wds and only bridges an interface that has it. Without this the child
#                associates, the apply reports success, and no 1905 frame ever crosses the radio
role_child() { printf 'slave' > "$DB/zte_mesh.main.mesh_role"; }
role_master() { rm -f "$DB/zte_mesh.main.mesh_role"; }

seed_db
backhaul_on
role_child
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "child: client section created" wifi-iface "$(val type.zte_backhaul_sta_2g)"
check "child: mode is sta" sta "$(val zte_backhaul_sta_2g.mode)"
check "child: the flag that makes it a backhaul sta" 1 "$(val zte_backhaul_sta_2g.multi_ap)"
check "child: 4-address and bridged (wds)" 1 "$(val zte_backhaul_sta_2g.wds)"
check "child: name is the controller's" ZMESH-bh-2g "$(val zte_backhaul_sta_2g.ssid)"
check "child: key is the controller's" backhaul-2g-key "$(val zte_backhaul_sta_2g.key)"
check "child: security (beacon 6)" psk-mixed "$(val zte_backhaul_sta_2g.encryption)"
check "child: radio borrowed from the band" radio0 "$(val zte_backhaul_sta_2g.device)"
check "child: network is the band's bridge" lan "$(val zte_backhaul_sta_2g.network)"
check "child: enabled so the link is up" 0 "$(val zte_backhaul_sta_2g.disabled)"
check "child: no AP-only hidden flag invented" "" "$(val zte_backhaul_sta_2g.hidden)"
check "child: no backhaul BSS of its own" "" "$(val type.zte_backhaul_2g)"
check "child: the band's main BSS is still the fronthaul" zte-agent-2g "$(val default_radio0.ssid)"
check "child: fronthaul of the main BSS marked" 1 "$(val default_radio0.multi_ap)"
check "child: no client invented for a band the controller did not fill" "" "$(val type.zte_backhaul_sta_5g)"

# A radio that carries no main BSS has no bridge to join: this image hands a sta interface to the network only
# through wds + network, so the client would associate and still not carry a 1905 frame. The name of the missing
# piece goes to the log -- a silent unbridged client reads as a working link on the page.
seed_db
rm -f "$DB/type.default_radio0" "$DB"/default_radio0.*
backhaul_on
role_child
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "child without a main BSS: client still written" wifi-iface "$(val type.zte_backhaul_sta_2g)"
check "child without a main BSS: no network invented" "" "$(val zte_backhaul_sta_2g.network)"
check "child without a main BSS: the log says it is not bridged" 1 \
	"$(grep -c 'no network to join (this radio carries no main BSS); it will not be bridged' "$LOG")"

# The same plan again is a no-op: the child must not be re-associated every two seconds.
seed_db
backhaul_on
role_child
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "child: second run sets nothing" 0 "$(grep -c '^set ' "$LOG")"
check "child: second run commits nothing" 0 "$(grep -c '^commit ' "$LOG")"
check "child: second run reloads nothing" 0 "$(grep -c '^wifi ' "$LOG")"

# Changing end while the mode stays on gives the other end's section back -- the plan row did not change, this
# router's role did, and a stale backhaul BSS on a child (or a stale client on a master) would both conflict with
# the controller and read as a working link on the page.
sh "$WLAN_SYNC" "$PLAN_BH" --once >/dev/null 2>&1
role_master
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "sta -> ap: the client section is gone" "" "$(val type.zte_backhaul_sta_2g)"
check "sta -> ap: the BSS section is built" wifi-iface "$(val type.zte_backhaul_2g)"
check "sta -> ap: removal says why" 1 "$(grep -c 'ap role runs the other end of the backhaul; removed zte_backhaul_sta_2g' "$LOG")"
role_child
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "ap -> sta: the BSS section is gone" "" "$(val type.zte_backhaul_2g)"
check "ap -> sta: the client section is built" wifi-iface "$(val type.zte_backhaul_sta_2g)"

# A row that claims a security mode but carries no key is refused at either end. The master would otherwise put an
# open BSS on the air under a name that is supposed to be private; the child would be configured to join a network
# it cannot authenticate to, and the page would show a backhaul that was never going to come up.
sed -e 's/^1.key=backhaul-2g-key$/1.key=/' "$PLAN_BH" > "$T/plan-bh4"
seed_db
backhaul_on
role_child
sh "$WLAN_SYNC" "$T/plan-bh4" --once >"$T/out" 2>&1
check "child: a secured row with no key is refused" "" "$(val type.zte_backhaul_sta_2g)"
check "child: the refusal says what is missing" 1 "$(grep -c 'says psk-mixed but the plan carries no key' "$LOG")"
check "child: the refusal does not stop the main BSS" zte-agent-2g "$(val default_radio0.ssid)"
seed_db
backhaul_on
role_master
sh "$WLAN_SYNC" "$T/plan-bh4" --once >"$T/out" 2>&1
check "master: a secured row with no key is refused" "" "$(val type.zte_backhaul_2g)"

# An open backhaul (beacon 0) has no key to miss, so it is still applied -- and the key it does not have is not
# written as an empty one.
sed -e 's/^1.beacon_type=6$/1.beacon_type=0/' -e 's/^1.key=backhaul-2g-key$/1.key=/' "$PLAN_BH" > "$T/plan-bh5"
seed_db
backhaul_on
role_child
sh "$WLAN_SYNC" "$T/plan-bh5" --once >"$T/out" 2>&1
check "child: an open backhaul is still applied" wifi-iface "$(val type.zte_backhaul_sta_2g)"
check "child: open backhaul security" none "$(val zte_backhaul_sta_2g.encryption)"
check "child: open backhaul carries no empty key" "" "$(val zte_backhaul_sta_2g.key)"

# A row the controller disables is written but kept off, so the child does not associate to a network the
# controller has stopped intending.
sed -e 's/^1.enabled=1$/1.enabled=0/' "$PLAN_BH" > "$T/plan-bh6"
seed_db
backhaul_on
role_child
sh "$WLAN_SYNC" "$T/plan-bh6" --once >"$T/out" 2>&1
check "child: a disabled row is applied as off" 1 "$(val zte_backhaul_sta_2g.disabled)"

# The role decides the end, nothing else: an unset role is a master, and a name this script has never seen
# (`main`, `auto`) stays a master too.
seed_db
backhaul_on
printf 'main' > "$DB/zte_mesh.main.mesh_role"
sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "role main is still the master end" wifi-iface "$(val type.zte_backhaul_2g)"
check "role main builds no client" "" "$(val type.zte_backhaul_sta_2g)"


# --- extra SSID slots (zte_mesh.main.extra_slots) ---------------------------------------------------------
# The controller hands out up to three extra SSIDs per band (slot 1 guest, 2 spare, 3 hidden backhaul) and the
# plan carries one row per slot. Off is the default and must stay byte-for-byte the old behaviour; on turns each
# enabled slot into its own wifi-iface on the band's radio, and removes the section again when the slot goes away.
extra_on() { printf '1' > "$DB/zte_mesh.main.extra_slots"; }
extra_off() { rm -f "$DB/zte_mesh.main.extra_slots"; }

PLAN_SLOTS=$T/plan-slots
cat > "$PLAN_SLOTS" <<'PLAN'
rows=3
0.radio=0
0.slot=0
0.instance=DEV.WIFI.AP1
0.ssid=zte-agent-2g
0.key=m2-public-2g-key
0.enabled=1
0.hidden=0
0.access=32
0.beacon_type=6
1.radio=0
1.slot=1
1.instance=DEV.WIFI.AP2
1.ssid=guest-2g
1.key=guest-2g-key
1.enabled=1
1.hidden=0
1.access=32
1.beacon_type=3
2.radio=1
2.slot=2
2.instance=DEV.WIFI.AP7
2.ssid=spare-5g
2.key=spare-5g-key
2.enabled=0
2.hidden=1
2.access=32
2.beacon_type=3
PLAN

# Off (the default): the slots are reported and skipped, exactly as before this option existed.
seed_db
extra_off
sh "$WLAN_SYNC" "$PLAN_SLOTS" --once >"$T/out" 2>&1
check "slots off: no extra section" "" "$(val type.zte_extra_2g_1)"
check "slots off: nothing created for the disabled slot either" "" "$(val type.zte_extra_5g_2)"
check "slots off: the row is reported" 1 "$(grep -c 'slot 1 of radio 0 is not applied' "$LOG")"
check "slots off: the log names the option" yes "$(has 'zte_mesh.main.extra_slots is off')"
check "slots off: the main BSS is still applied" zte-agent-2g "$(val default_radio0.ssid)"

# On: an enabled slot becomes its own BSS on its band's radio.
seed_db
extra_on
sh "$WLAN_SYNC" "$PLAN_SLOTS" --once >"$T/out" 2>&1
check "slot section type" wifi-iface "$(val type.zte_extra_2g_1)"
check "slot radio" radio0 "$(val zte_extra_2g_1.device)"
check "slot mode" ap "$(val zte_extra_2g_1.mode)"
check "slot name is the controller's" guest-2g "$(val zte_extra_2g_1.ssid)"
check "slot key is the controller's" guest-2g-key "$(val zte_extra_2g_1.key)"
check "slot security (beacon 3)" psk2 "$(val zte_extra_2g_1.encryption)"
check "slot enabled" 0 "$(val zte_extra_2g_1.disabled)"
check "slot hidden flag" 0 "$(val zte_extra_2g_1.hidden)"
check "slot inherits the band's network" lan "$(val zte_extra_2g_1.network)"
check "slot is not a backhaul BSS" "" "$(val zte_extra_2g_1.multi_ap)"
check "slot row does not touch the main SSID" zte-agent-2g "$(val default_radio0.ssid)"
check "a disabled slot creates nothing" "" "$(val type.zte_extra_5g_2)"
check "one commit for the whole plan" 1 "$(grep -c '^commit wireless$' "$LOG")"
check "the apply says the guest slot is not separated" 1 "$(grep -c 'no VLAN separation' "$LOG")"

# Idempotent: the same plan again writes, commits and reloads nothing.
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN_SLOTS" --once >"$T/out" 2>&1
check "second slot run sets nothing" 0 "$(grep -c '^set ' "$LOG")"
check "second slot run commits nothing" 0 "$(grep -c '^commit ' "$LOG")"
check "second slot run reloads nothing" 0 "$(grep -c '^wifi ' "$LOG")"

# A slot the plan no longer carries is removed: the child must not keep broadcasting a network the controller
# stopped handing out. A slot that is merely disabled is the row's job, not the pruner's.
cat > "$T/plan-slots-none" <<'PLAN'
rows=2
0.radio=0
0.slot=0
0.ssid=zte-agent-2g
0.key=m2-public-2g-key
0.enabled=1
0.hidden=0
0.access=32
0.beacon_type=6
1.radio=1
1.slot=2
1.ssid=spare-5g
1.key=spare-5g-key
1.enabled=0
1.hidden=1
1.access=32
1.beacon_type=3
PLAN
: > "$LOG"
sh "$WLAN_SYNC" "$T/plan-slots-none" --once >"$T/out" 2>&1
check "slot gone: section removed" "" "$(val type.zte_extra_2g_1)"
check "slot gone: removal logged" 1 "$(grep -c 'removed zte_extra_2g_1' "$LOG")"
check "slot gone: the main BSS stays" zte-agent-2g "$(val default_radio0.ssid)"

# Switching the mode back to off removes every extra section this script created, and nothing else.
seed_db
extra_on
sh "$WLAN_SYNC" "$PLAN_SLOTS" --once >"$T/out" 2>&1
extra_off
: > "$LOG"
sh "$WLAN_SYNC" "$PLAN_SLOTS" --once >"$T/out" 2>&1
check "off again: section removed" "" "$(val type.zte_extra_2g_1)"
check "off again: removal logged" 1 "$(grep -c 'extra slot mode is off; removed 1' "$LOG")"
check "off again: the main BSS untouched" zte-agent-2g "$(val default_radio0.ssid)"

# An unknown security mode creates nothing; a slot beyond 3 is refused instead of guessed at.
sed -e 's/^1.beacon_type=3$/1.beacon_type=9/' "$PLAN_SLOTS" > "$T/plan-slots-bad"
seed_db
extra_on
sh "$WLAN_SYNC" "$T/plan-slots-bad" --once >"$T/out" 2>&1
check "unknown slot mode creates nothing" "" "$(val type.zte_extra_2g_1)"
check "unknown slot mode logged" 1 "$(grep -c 'slot 1 on 2g: unknown security mode 9' "$LOG")"

sed -e 's/^1.slot=1$/1.slot=4/' "$PLAN_SLOTS" > "$T/plan-slots-range"
seed_db
extra_on
sh "$WLAN_SYNC" "$T/plan-slots-range" --once >"$T/out" 2>&1
check "slot 4 refused" "" "$(val type.zte_extra_2g_4)"
check "slot 4 logged" 1 "$(grep -c 'slot 4 of radio 0 is out of range' "$LOG")"

# A slot row without a name is left alone (an empty SSID would create an unnamed, unconnectable BSS).
sed -e 's/^1.ssid=guest-2g$/1.ssid=/' "$PLAN_SLOTS" > "$T/plan-slots-noname"
seed_db
extra_on
sh "$WLAN_SYNC" "$T/plan-slots-noname" --once >"$T/out" 2>&1
check "slot without a name creates nothing" "" "$(val type.zte_extra_2g_1)"
check "slot without a name logged" 1 "$(grep -c 'slot 1 of radio 0 has no name' "$LOG")"

# An extra slot on a band whose radio cannot be resolved is reported and skipped, not written to a guess. This is
# the ax3600 shape: two '5g' devices, neither pinned, so the band is ambiguous.
cat > "$T/plan-slots-5g" <<'PLAN'
rows=1
0.radio=1
0.slot=1
0.ssid=guest-5g
0.key=guest-5g-key
0.enabled=1
0.hidden=0
0.access=32
0.beacon_type=3
PLAN
seed_db_two_5g
extra_on
sh "$WLAN_SYNC" "$T/plan-slots-5g" --once >"$T/out" 2>&1
check "ambiguous band: no section" "" "$(val type.zte_extra_5g_1)"
check "ambiguous band: reported" 1 "$(grep -c 'no usable 5g radio; slot 1 left alone' "$LOG")"
check "ambiguous band: the old main SSID stays" old-card-5g "$(val default_radio0.ssid)"

# Every run leaves a status file next to the plan (wlan-sync.status).
seed_db
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "status: result" ok "$(status_val result)"
check "status: an apply counts as changed" 1 "$(status_val changed)"
check "status: both bands named" "2g 5g" "$(status_val bands)"
check "status: epoch is a number" 1 "$(status_val epoch | grep -c '^[0-9][0-9]*$')"

# The same plan again changes nothing: the bands field must be empty, not stale.
sh "$WLAN_SYNC" "$PLAN" --once >"$T/out" 2>&1
check "status: a no-op run reports no change" 0 "$(status_val changed)"
check "status: a no-op run names no band" "" "$(status_val bands)"
check "status: still ok" ok "$(status_val result)"

# --- the installed wpad variant --------------------------------------------------------------------------
# The backhaul options this script writes (`multi_ap` on either end, and the `wds` that opens 4-address mode on a
# client) exist only in the full wpad build: a -basic package accepts them and puts nothing on the air. A run that
# reported a clean success in that state would be the exact kind of false pass this project keeps having, so the
# reason goes to the log and into the status message the page reads. WPAD_DB points the lookup at a file, so the
# test does not depend on the host.
printf 'P:wpad-basic-mbedtls\nP:libc\n' > "$T/pkg-basic"
printf 'Package: wpad-mbedtls\n' > "$T/pkg-full"
seed_db
backhaul_on
: > "$LOG"
WPAD_DB=$T/pkg-basic sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "wpad basic: warning logged" 1 "$(grep -c 'wpad-basic-mbedtls has no Multi-AP support' "$LOG")"
check "wpad basic: warning in the status" 1 "$(status_val message | grep -c 'wpad-basic-mbedtls has no Multi-AP support')"
check "wpad basic: still applied" 2 "$(val zte_backhaul_2g.multi_ap)"

# The client end is the same package question and the same false pass: -basic takes multi_ap and wds, associates,
# and carries no 1905 frame.
seed_db
backhaul_on
role_child
: > "$LOG"
WPAD_DB=$T/pkg-basic sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "child: wpad basic warned too" 1 "$(grep -c 'wpad-basic-mbedtls has no Multi-AP support' "$LOG")"
check "child: wpad basic still writes the 4-address client" 1 "$(val zte_backhaul_sta_2g.wds)"
check "child: wpad basic names it in the status" 1 "$(status_val message | grep -c 'wpad-basic-mbedtls has no Multi-AP support')"

seed_db
backhaul_on
: > "$LOG"
WPAD_DB=$T/pkg-full sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "full wpad: no warning" "" "$(status_val message)"

# The warning belongs to the backhaul: with it off, a -basic image is perfectly fine.
seed_db
backhaul_off
: > "$LOG"
WPAD_DB=$T/pkg-basic sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "backhaul off: no wpad warning" "" "$(status_val message)"

# An unreadable package database is not an error: nothing installed is known, so nothing is claimed.
seed_db
backhaul_on
: > "$LOG"
WPAD_DB=$T/no-such-file sh "$WLAN_SYNC" "$PLAN_BH" --once >"$T/out" 2>&1
check "no package database: no warning, no failure" "" "$(status_val message)"
check "no package database: still ok" ok "$(status_val result)"

echo "wlan-sync: $pass passed, $fail failed"
[ "$fail" = 0 ]
