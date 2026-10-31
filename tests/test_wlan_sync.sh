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


# Every run leaves a status file next to the plan (wlan-sync.status): the sandboxed service and the page cannot see
# what happens outside the sandbox, and without it a failed apply is only a line in the system log.
status_val() { sed -n "s/^$1=//p" "$T/wlan-sync.status" 2>/dev/null | head -n 1; }
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

echo "wlan-sync: $pass passed, $fail failed"
[ "$fail" = 0 ]
