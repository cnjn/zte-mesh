#!/bin/sh
# Host test of openwrt/zte-mesh/files/sta-collect: the two judgements it makes -- which of netifd's interfaces is an
# access point, and what a station count is worth reporting for one -- and the shape of the table it hands to the
# sandbox.
#
# ubus, jsonfilter and date are stubs, and /sys/class/net plus the mac80211 debugfs tree are directories under a
# temporary root: the script takes all five from the environment, so nothing here reads the machine it runs on. The
# fixture lines are jsonfilter's own output shape, taken from an ax3600, spacing included -- that spacing is what the
# script has to survive, so it is not normalised away here.
#
# Run with the shell OpenWrt uses: busybox ash tests/test_sta_collect.sh
#
#   sh tests/test_sta_collect.sh [STA_COLLECT]
STA_COLLECT=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/sta-collect}
[ -f "$STA_COLLECT" ] || { echo "sta-collect not found: $STA_COLLECT" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
BIN=$T/bin
FIX=$T/fix
RUN=$T/run
NET=$T/net
DBG=$T/debug
LOG=$T/log
TABLE=$RUN/ap-sta
mkdir -p "$BIN" "$FIX" "$RUN" "$NET" "$DBG"

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}
rows() { sed -n '2,$p' "$TABLE" 2>/dev/null; }
log_has() { grep -c "$1" "$LOG" 2>/dev/null; }

printf '1700000000\n' > "$FIX/epoch"

# The three commands the script reads through, and the tree it counts in, come from the environment.
cat > "$BIN/ubus" <<'STUB'
#!/bin/sh
# netifd's answer, Wi-Fi key included: what the collector reads here must never reach the table or the log.
cat "$ZTE_MESH_FIX/wireless-status"
STUB
cat > "$BIN/jsonfilter" <<'STUB'
#!/bin/sh
cat > /dev/null
printf '%s\n' "$*" >> "$ZTE_MESH_FIX/jsonfilter.args"
[ -f "$ZTE_MESH_FIX/ifaces" ] && cat "$ZTE_MESH_FIX/ifaces"
STUB
cat > "$BIN/date" <<'STUB'
#!/bin/sh
cat "$ZTE_MESH_FIX/epoch"
STUB
cat > "$BIN/logger" <<'STUB'
#!/bin/sh
printf '%s\n' "$5" >> "$ZTE_MESH_LOG"
STUB
chmod +x "$BIN"/*

# Two access points and the backhaul client the same child runs: phy1-sta0 has stations of its own -- the controller
# on the other end -- and counting it would report that router as a terminal of this one.
cat > "$FIX/ifaces" <<'FIX'
{ "section": "default_radio1", "config": { "network": [ "lan" ], "mode": "ap", "key": "fixture-not-a-key", "ssid": "FIX2G" }, "ifname": "phy1-ap0", "vlans": [ ] }
{ "section": "default_radio2", "config": { "mode": "ap", "ssid": "FIX5G" }, "ifname": "phy2-ap0", "vlans": [ ] }
{ "section": "backhaul", "config": { "mode": "sta", "key": "fixture-not-a-key" }, "ifname": "phy1-sta0", "vlans": [ ] }
FIX
cat > "$FIX/wireless-status" <<'FIX'
{ "radio1": { "interfaces": [ { "ifname": "phy1-ap0" } ] } }
FIX

mac_of() { mkdir -p "$NET/$1"; printf '%s\n' "$2" > "$NET/$1/address"; }
sta_dir() { mkdir -p "$DBG/$1/netdev:$2/stations"; }
add_sta() { mkdir -p "$DBG/$1/netdev:$2/stations/$3"; }

mac_of phy1-ap0 aa:aa:aa:00:00:01
mac_of phy2-ap0 aa:aa:aa:00:00:02
mac_of phy1-sta0 aa:aa:aa:00:00:03
sta_dir phy0 phy1-ap0
add_sta phy0 phy1-ap0 11:22:33:44:55:66
add_sta phy0 phy1-ap0 77:88:99:aa:bb:cc
sta_dir phy1 phy2-ap0
sta_dir phy0 phy1-sta0
add_sta phy0 phy1-sta0 dd:dd:dd:dd:dd:dd

run() {
	: > "$LOG"
	rm -f "$FIX/jsonfilter.args"
	ZTE_MESH_UBUS=$BIN/ubus ZTE_MESH_JSONFILTER=$BIN/jsonfilter ZTE_MESH_CLOCK=$BIN/date \
	ZTE_MESH_NET="$NET" ZTE_MESH_MAC80211_DEBUG="$DBG" ZTE_MESH_FIX="$FIX" ZTE_MESH_LOG="$LOG" \
	PATH="$BIN:$PATH" sh "$STA_COLLECT" "$TABLE" 5 --once
}

# 1. the two access points, their measured counts, and the client left out.
run
check "epoch line" "epoch=1700000000" "$(sed -n 1p "$TABLE" 2>/dev/null)"
check "one row per access point" "aa:aa:aa:00:00:01 2
aa:aa:aa:00:00:02 0" "$(rows)"
check "backhaul client not counted" "0" "$(rows | grep -c 'aa:aa:aa:00:00:03')"
check "key never written" "0" "$(cat "$TABLE" | grep -ci 'fixture-not-a-key')"
check "startup log" "1" "$(log_has 'collecting 2 access point(s)')"
check "the expression the script asks jsonfilter for" "1" "$(grep -c -- '-e @.\*.interfaces\[\*\]' "$FIX/jsonfilter.args")"
check "no temporary table left behind" "ap-sta" "$(ls "$RUN" | tr '\n' ' ' | sed 's/ $//')"

# 2. an interface the kernel says nothing about is not a measured zero: no row at all.
rm -rf "$DBG/phy1/netdev:phy2-ap0"
run
check "no stations directory: no row" "aa:aa:aa:00:00:01 2" "$(rows)"
check "and the count said in the log matches" "1" "$(log_has 'collecting 1 access point(s)')"

# 3. no debugfs tree at all (a kernel built without mac80211 debug, or a mount that is gone) is an empty table, not a
#    failure -- the responder reads an empty table as "nothing measured" and says nothing.
DBG_BACKUP=$T/dbg-gone
run_missing_debug() {
	: > "$LOG"
	ZTE_MESH_UBUS=$BIN/ubus ZTE_MESH_JSONFILTER=$BIN/jsonfilter ZTE_MESH_CLOCK=$BIN/date \
	ZTE_MESH_NET="$NET" ZTE_MESH_MAC80211_DEBUG="$DBG_BACKUP" ZTE_MESH_FIX="$FIX" ZTE_MESH_LOG="$LOG" \
	PATH="$BIN:$PATH" sh "$STA_COLLECT" "$TABLE" 5 --once
	return $?
}
check "no debugfs: exit 0" "0" "$(run_missing_debug; echo $?)"
check "no debugfs: table holds only the epoch" "" "$(rows)"
check "no debugfs: nothing complained about" "0" "$(log_has 'cannot write')"

# 4. an access point with no MAC in /sys cannot be answered for; the other row still lands.
sta_dir phy1 phy2-ap0
mv "$NET/phy1-ap0/address" "$T/hidden-mac"
run
check "interface without an address: row skipped" "aa:aa:aa:00:00:02 0" "$(rows)"
mv "$T/hidden-mac" "$NET/phy1-ap0/address"

# 5. an object without an ifname is not a crash and not a row.
printf '%s\n' '{ "section": "default_radio1", "config": { "mode": "ap" } }' >> "$FIX/ifaces"
run
check "object without an ifname skipped" "aa:aa:aa:00:00:01 2
aa:aa:aa:00:00:02 0" "$(rows)"

# 6. a mode other than ap never reaches the table, whatever it is called.
printf '%s\n' '{ "section": "mesh", "config": { "mode": "mesh" }, "ifname": "phy1-mesh0" }' >> "$FIX/ifaces"
mac_of phy1-mesh0 aa:aa:aa:00:00:04
sta_dir phy0 phy1-mesh0
add_sta phy0 phy1-mesh0 ee:ee:ee:ee:ee:ee
run
check "mesh mode not an access point" "0" "$(rows | grep -c 'aa:aa:aa:00:00:04')"

# 7. the tools gone: a named file that cannot be exec'd is the same hole as a command that was never installed, and
#    the run says which one and keeps the old table out of the answer.
printf '%s\n' "epoch=1600000000
ab:ab:ab:ab:ab:ab 9" > "$TABLE"
no_tools() {
	: > "$LOG"
	ZTE_MESH_UBUS=$BIN/absent ZTE_MESH_JSONFILTER=$BIN/absent ZTE_MESH_CLOCK=$BIN/date \
	ZTE_MESH_NET="$NET" ZTE_MESH_MAC80211_DEBUG="$DBG" ZTE_MESH_FIX="$FIX" ZTE_MESH_LOG="$LOG" \
	PATH="$BIN:$PATH" sh "$STA_COLLECT" "$TABLE" 5 --once
	return $?
}
check "missing tools: exit 1" "1" "$(no_tools; echo $?)"
check "missing tools: names both" "1" "$(log_has 'no ubus, jsonfilter: no access point counts')"
check "missing tools: table untouched" "ab:ab:ab:ab:ab:ab 9" "$(rows)"

# 8. the run directory missing (the service stopped, or /run wiped under it) is the same judgement, said once.
no_rundir() {
	: > "$LOG"
	ZTE_MESH_UBUS=$BIN/ubus ZTE_MESH_JSONFILTER=$BIN/jsonfilter ZTE_MESH_CLOCK=$BIN/date \
	ZTE_MESH_NET="$NET" ZTE_MESH_MAC80211_DEBUG="$DBG" ZTE_MESH_FIX="$FIX" ZTE_MESH_LOG="$LOG" \
	PATH="$BIN:$PATH" sh "$STA_COLLECT" "$T/gone/ap-sta" 5 --once
	return $?
}
check "no run directory: exit 1" "1" "$(no_rundir; echo $?)"
check "no run directory: names it" "1" "$(log_has "no $T/gone: no access point counts")"

# 9. a collector that comes back rewrites the table whole: the rows of the run before it are not carried into the
#    answer, whatever they said.
printf '%s\n' "epoch=1600000000" "ab:ab:ab:ab:ab:ab 9" > "$TABLE"
run
check "a previous table is replaced, not appended to" "aa:aa:aa:00:00:01 2
aa:aa:aa:00:00:02 0" "$(rows)"
check "the epoch moves with the clock" "epoch=1700000000" "$(sed -n 1p "$TABLE")"

# 10. the pause flag stops this loop and nothing else: the last table stays where the responder can read it (and age),
#     the log says the stop once, and the run after the flag is gone says the collecting line again.
run
printf '%s\n' 'pause' > "$RUN/collect-pause"
: > "$LOG"
run
check "paused: the epoch does not move" "epoch=1700000000" "$(sed -n 1p "$TABLE")"
check "paused: rows kept" "aa:aa:aa:00:00:01 2
aa:aa:aa:00:00:02 0" "$(rows)"
check "paused: names the flag" "1" "$(log_has 'paused by .*collect-pause')"
check "paused: not collecting" "0" "$(log_has 'collecting')"
check "paused: exit 0" "0" "$(run; echo $?)"
# `run` clears the log first, so this counts one process: the gate lives in the loop, not on disk, and on the router
# the loop is one process -- which is the "said once" the script promises.
check "paused twice: one stop line per process" "1" "$(log_has 'paused by')"
check "paused twice: still the same table" "epoch=1700000000
aa:aa:aa:00:00:01 2
aa:aa:aa:00:00:02 0" "$(cat "$TABLE")"
rm -f "$RUN/collect-pause"
: > "$LOG"
printf '1800000000\n' > "$FIX/epoch"
run
check "resumed: the epoch moves again" "epoch=1800000000" "$(sed -n 1p "$TABLE")"
check "resumed: says it is collecting" "1" "$(log_has 'collecting 2 access point(s)')"
check "resumed: no pause line left" "0" "$(log_has 'paused by')"
printf '1700000000\n' > "$FIX/epoch"

echo "sta-collect: $pass passed, $fail failed"
[ "$fail" = 0 ]
