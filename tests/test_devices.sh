#!/bin/sh
# Host test of openwrt/zte-mesh/files/device-list: the merge of the three local tables, the judgement about which
# neighbours are devices, and the shape of the JSON the page relays.
#
# The script takes every external command and every path it reads from the environment, so this test answers from
# fixtures: `ip`, `ubus`, `jsonfilter` and `date` are shell functions of the shell that sources the script (nothing
# under $T has to be executable -- the container that runs make test-host mounts /tmp noexec), and the lease file,
# the mac80211 debugfs tree and the run directory are files under a temporary root.
#
# The fixture lines are the real output shapes: /tmp/dhcp.leases and `ip neigh` from an ax3600, and jsonfilter's own
# spaced rendering of one wireless interface object. An SSID with a space stays in the fixture, because normalising
# that spacing away is exactly what would hide the bug the script has to avoid.
#
# Run with the shell OpenWrt uses: busybox ash tests/test_devices.sh
#
#   sh tests/test_devices.sh [DEVICE_LIST]
DEVICE_LIST=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/device-list}
[ -f "$DEVICE_LIST" ] || { echo "device-list not found: $DEVICE_LIST" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
FIX=$T/fix
RUN=$T/run
DBG=$T/debug
mkdir -p "$FIX" "$RUN" "$DBG"

PY=$(command -v python3 || command -v python) || { echo "test_devices: no python3/python" >&2; exit 1; }

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}

# One line per device plus the epoch, so a failing case prints what it got instead of a wall of JSON.
rows() {
	"$PY" -c '
import json, sys
sys.stdout.reconfigure(newline="\n")   # a test run on Windows would otherwise compare \r\n against \n
d = json.load(sys.stdin)
for x in d["devices"]:
	print("|".join([x["mac"], x["ip"], x["name"] or "-", str(x["lease_end"]), x["iface"] or "-",
	                x["ssid"] or "-", x["sources"] or "-", "node" if x["node"] else "-", x["via"] or "-"]))
print("epoch=%d" % d["epoch"])
'
}
valid() { "$PY" -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; echo $?; }

# The fixtures. Rewritten per case so one case cannot leave a line behind in the next one.
leases() {
	cat > "$FIX/leases"
}
ifaces() {
	printf '%s\n' "$@" > "$FIX/ifaces"
}
neigh() {
	printf '%s\n' "$@" > "$FIX/neigh"
}
# The radio: phy1-ap0 is an access point, phy1-sta0 is the backhaul client a child builds toward its controller.
ap_fixture() {
	IFN=$1
	mkdir -p "$DBG/phy0/netdev:$IFN/stations" "$DBG/phy1/netdev:$IFN/stations"
}
add_sta() { mkdir -p "$DBG/phy0/netdev:$1/stations/$2"; }

run() {
	ZTE_MESH_LEASE=$FIX/leases ZTE_MESH_BRIDGE_IF=br-lan ZTE_MESH_MAC80211_DEBUG=$DBG ZTE_MESH_RUNDIR=$RUN \
	ZTE_MESH_IP=ip ZTE_MESH_UBUS=ubus ZTE_MESH_JSONFILTER=jsonfilter ZTE_MESH_CLOCK=date \
	ZTE_MESH_SCRIPT=$DEVICE_LIST FIX=$FIX \
	sh -c '
		ip() { cat "$FIX/neigh"; }
		ubus() { cat "$FIX/wireless-status"; }
		jsonfilter() {
			while [ $# -gt 0 ]; do
				case $1 in
				-i) shift 2 ;;
				-e) EXPR=$2; shift 2 ;;
				*) shift ;;
				esac
			done
			case $EXPR in
			"@.*.interfaces[*]") cat "$FIX/ifaces" ;;
			"@.nodes[*].mac") cat "$FIX/nodes" 2>/dev/null ;;
			esac
		}
		date() { cat "$FIX/epoch"; }
		. "$ZTE_MESH_SCRIPT"
	'
}

printf '1700000000\n' > "$FIX/epoch"
: > "$FIX/nodes"
printf '{"nodes":[]}\n' > "$RUN/state.json"
printf '%s\n' '{ "radio1": { "interfaces": [ { "ifname": "phy1-ap0" } ] } }' > "$FIX/wireless-status"

# 1. The three tables merge by MAC. A wired PC is a lease and a neighbour; the phone on the radio is a lease, a
#    neighbour and a station; the printer with a static address is a neighbour and nothing else -- no lease is exactly
#    what a static address looks like, and leaving it out would make the page disagree with the network.
leases <<'FIX'
1794221941 00:01:f3:96:43:cf 192.168.1.162 DESKTOP-JSJGOJL 01:00:01:f3:96:43:cf
1794222400 0a:95:b7:16:e2:39 192.168.1.221 Xiaomi-15 *
FIX
neigh '192.168.1.162 dev br-lan lladdr 00:01:f3:96:43:cf REACHABLE' \
      '192.168.1.222 dev br-lan lladdr 0a:95:b7:16:e2:39 STALE' \
      '192.168.1.50 dev br-lan lladdr 3C:84:6A:00:11:22 STALE' \
      '192.168.1.99 dev br-lan lladdr 00:00:00:00:00:99 FAILED' \
      '192.168.1.98 dev br-lan  FAILED'
ifaces '{ "section": "default_radio1", "config": { "network": [ "lan" ], "mode": "ap", "key": "fixture-not-a-key", "ssid": "FIX2G" }, "ifname": "phy1-ap0", "vlans": [ ] }' \
       '{ "section": "backhaul", "config": { "mode": "sta", "key": "fixture-not-a-key" }, "ifname": "phy1-sta0", "vlans": [ ] }'
ap_fixture phy1-ap0
ap_fixture phy1-sta0
add_sta phy1-ap0 0a:95:b7:16:e2:39
add_sta phy1-sta0 dd:dd:dd:dd:dd:dd

check "valid JSON" 0 "$(run | valid)"
check "merged rows" "00:01:f3:96:43:cf|192.168.1.162|DESKTOP-JSJGOJL|1794221941|-|-|lease neigh|-|-
0a:95:b7:16:e2:39|192.168.1.222|Xiaomi-15|1794222400|phy1-ap0|FIX2G|lease neigh wlan|-|-
3c:84:6a:00:11:22|192.168.1.50|-|0|-|-|neigh|-|-
epoch=1700000000" "$(run | rows)"
check "the key never leaves the script" "0" "$(run | grep -ci 'fixture-not-a-key')"

# 2. The live address wins over the lease's while the lease's expiry still counts: the phone moved from .221 to .222
#    and the lease file still holds the old address.
check "live address, lease expiry kept" "0a:95:b7:16:e2:39|192.168.1.222|Xiaomi-15|1794222400" \
	"$(run | rows | sed -n 2p | cut -d'|' -f1-4)"

# 3. The backhaul client is a router on the other end of a link, not a device of this router: mode=sta is not an
#    access point, whatever stations directory it has.
check "backhaul station left out" "0" "$(run | rows | grep -c 'dd:dd:dd:dd:dd:dd')"

# 4. A router the controller knows in its topology is marked as one, so a child is not read as a device.
printf 'd4:b7:09:0d:77:30\n' > "$FIX/nodes"
neigh '192.168.1.162 dev br-lan lladdr 00:01:f3:96:43:cf REACHABLE' \
      '192.168.1.171 dev br-lan lladdr D4:B7:09:0D:77:30 REACHABLE'
check "known router marked" "d4:b7:09:0d:77:30|192.168.1.171|-|0|-|-|neigh|node|-" "$(run | rows | grep 'd4:b7')"
: > "$FIX/nodes"

# 5. No debugfs tree at all (a kernel built without mac80211 debug, or a mount that is gone): the two other tables
#    still answer.
DBG_BACKUP=$T/debug-backup
mv "$DBG" "$DBG_BACKUP"
mkdir -p "$DBG"
check "no debugfs: wired rows still there" "00:01:f3:96:43:cf|192.168.1.162|DESKTOP-JSJGOJL|1794221941" \
	"$(run | rows | sed -n 1p | cut -d'|' -f1-4)"
rm -rf "$DBG"
mv "$DBG_BACKUP" "$DBG"

# 6. No lease file (a router that runs no DHCP, or one whose leases are kept elsewhere): the other two tables stand.
mv "$FIX/leases" "$T/leases-backup"
check "no leases: neighbour and station rows remain" "3" "$(run | rows | grep -c '|')"
mv "$T/leases-backup" "$FIX/leases"

# 7. An empty network is a measured answer: no rows, the epoch still there, valid JSON, exit 0.
: > "$FIX/neigh"
: > "$FIX/leases"
rm -rf "$DBG/phy0/netdev:phy1-ap0/stations"
check "empty: valid JSON" 0 "$(run | valid)"
check "empty: no rows" "epoch=1700000000" "$(run | rows)"

# 8. A host name and an SSID are arbitrary bytes: a quote or a backslash in the host name cannot end the JSON string
#    early, and an SSID in UTF-8 with a space comes through as it is.
neigh '192.168.1.163 dev br-lan lladdr 00:01:f3:96:43:d0 REACHABLE'
printf '1794221941 00:01:f3:96:43:d0 192.168.1.163 bad"name\\here 01:00:01:f3:96:43:d0\n' > "$FIX/leases"
ifaces '{ "section": "default_radio1", "config": { "mode": "ap", "ssid": "中文 网" }, "ifname": "phy1-ap0", "vlans": [ ] }'
add_sta phy1-ap0 00:01:f3:96:43:d0
check "quoted host name: valid JSON" 0 "$(run | valid)"
check "quoted host name: the quote is dropped" "00:01:f3:96:43:d0|192.168.1.163|badnamehere|1794221941|phy1-ap0|中文 网|lease neigh wlan|-|-" \
	"$(run | rows | sed -n 1p)"

# 9. A child's own report: the phone is on that child's radio, so it is wireless and the Node column can name the
#    child, even though the only tables here that know its address are the lease and the neighbour table. A client
#    the child reports that has no address here is a device of the mesh too -- that is what "cross-node" means.
: > "$FIX/leases"
neigh '192.168.1.221 dev br-lan lladdr 0a:95:b7:16:e2:39 REACHABLE'
rm -rf "$DBG/phy0/netdev:phy1-ap0/stations"
mkdir -p "$RUN/peers"
printf 'epoch=1700000000\n0a:95:b7:16:e2:39\t02:00:00:00:19:e3\tFRESHR10-5G\naa:bb:cc:dd:ee:ff\t02:00:00:00:19:e3\t\n' > "$RUN/peers/02:00:00:00:19:e3.assoc"
check "child report: phone is wireless on that child" \
	"0a:95:b7:16:e2:39|192.168.1.221|-|0|-|FRESHR10-5G|neigh assoc|-|02:00:00:00:19:e3" \
	"$(run | rows | grep 0a:95)"
check "child report: a client with no address here is still a device of the mesh" \
	"aa:bb:cc:dd:ee:ff|-|-|0|-|-|assoc|-|02:00:00:00:19:e3" \
	"$(run | rows | grep aa:bb)"

# 10. A report is only worth what its stamp says it is: a node that stopped answering, one whose file carries a
#     time this clock has not reached, and a file whose name is not a node's MAC are all "not reported", not "nobody".
printf 'epoch=1699999950\n0a:95:b7:16:e2:39\t02:00:00:00:19:e3\tFRESHR10-5G\n' > "$RUN/peers/02:00:00:00:19:e3.assoc"
check "stale report ignored" "0" "$(run | rows | grep -c 'assoc')"
printf 'epoch=1700000060\n0a:95:b7:16:e2:39\t02:00:00:00:19:e3\tFRESHR10-5G\n' > "$RUN/peers/02:00:00:00:19:e3.assoc"
check "future stamp ignored" "0" "$(run | rows | grep -c 'assoc')"
printf 'epoch=1700000000\n0a:95:b7:16:e2:39\t02:00:00:00:19:e3\tFRESHR10-5G\n' > "$RUN/peers/not-a-mac.assoc"
check "file name that is not a MAC ignored" "0" "$(run | rows | grep -c 'assoc')"
rm -f "$RUN/peers/not-a-mac.assoc"

# 11. Two children can both list the phone for a moment while it roams: the report that arrived last wins, so the
#     page does not show one node by accident of file order.
printf 'epoch=1699999990\n0a:95:b7:16:e2:39\t02:00:00:00:19:e3\tOLD\n' > "$RUN/peers/02:00:00:00:19:e3.assoc"
printf 'epoch=1699999999\n0a:95:b7:16:e2:39\t02:00:00:00:19:20\tNEW\n' > "$RUN/peers/02:00:00:00:19:20.assoc"
check "the newest report names the node" \
	"0a:95:b7:16:e2:39|192.168.1.221|-|0|-|NEW|neigh assoc|-|02:00:00:00:19:20" \
	"$(run | rows | grep 0a:95)"

# 12. A station this router has in front of it is on this router, whatever a child reported a moment earlier: the
#     local interface and SSID are shown and the Node column stays empty.
add_sta phy1-ap0 0a:95:b7:16:e2:39
check "local association wins over a child report" \
	"0a:95:b7:16:e2:39|192.168.1.221|-|0|phy1-ap0|中文 网|neigh wlan assoc|-|-" \
	"$(run | rows | grep 0a:95)"

echo "device-list: $pass passed, $fail failed"
[ "$fail" = 0 ]
