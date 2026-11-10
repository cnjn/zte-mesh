#!/bin/sh
# Check the frames controller-steer.c builds against the Wireshark ieee1905 dissector.
#
#   sh tests/test_steer_tshark.sh [controller-steer-codec] [tshark]
#
# The C test already compares the frames with a byte literal, but a literal only proves the code still does
# what it did the day it was written. This proves the bytes mean what we think they mean: every field below is
# read back by the dissector, which is an independent implementation of the same message.
#
# tshark is not part of the CI image and Wireshark is a Windows program on the development host, so a missing
# tshark prints SKIP and exits 0. Run it where tshark exists (TSHARK=/path/to/tshark).
set -u
BIN=${1:-out/tests/controller-steer-codec}
TSHARK=${2:-${TSHARK:-tshark}}
pass=0
fail=0

check() { # name expected actual
	if [ "$2" = "$3" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL %s: expected %s, got %s\n' "$1" "$2" "$3"
	fi
}

[ -x "$BIN" ] || BIN=./$BIN
if [ ! -x "$BIN" ]; then
	printf 'SKIP: %s is not built\n' "$BIN"
	exit 0
fi
if ! command -v "$TSHARK" >/dev/null 2>&1; then
	printf 'SKIP: no tshark on this host (set TSHARK=/path/to/tshark)\n'
	exit 0
fi

T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT

"$BIN" --pcap "$T/steer.pcap" >/dev/null || { echo "FAIL: the codec could not write a pcap"; exit 1; }

fields() { # frame filter field
	"$TSHARK" -r "$T/steer.pcap" -Y "frame.number == $1" -T fields -e "$2" 2>/dev/null
}

# --- frame 1: 0x8014 Client Steering Request, one target BSS -------------------------------
check "1 is a steering request" 0x8014 "$(fields 1 ieee1905.message_type)"
check "1 client" aa:bb:cc:dd:ee:ff "$(fields 1 ieee1905.client_info.mac_addr)"
check "1 source BSS" 0c0e5a1b2c3d "$(fields 1 ieee1905.steering_req.source_bssid)"
check "1 flags" 0xc0 "$(fields 1 ieee1905.steering_req.flags)"
check "1 window" 200 "$(fields 1 ieee1905.steering_req.window)"
check "1 disassociation timer" 100 "$(fields 1 ieee1905.steering_req.disass_timer)"
check "1 target MAC" aa:bb:cc:dd:ee:ff "$(fields 1 ieee1905.steering_req.target_mac)"
check "1 target BSSID" 0c0e5a1b2c44 "$(fields 1 ieee1905.steering_req.target_bssid)"
check "1 operating class" 81 "$(fields 1 ieee1905.steering_req.oper_class)"
check "1 channel" 24 "$(fields 1 ieee1905.steering_req.target_channel)"

# --- frame 2: 0x8016 Client Association Control Request, two clients blocked for an hour ---
check "2 is an association control request" 0x8016 "$(fields 2 ieee1905.message_type)"
check "2 control is block" 0x00 "$(fields 2 ieee1905.assoc_ctrl.control)"
check "2 validity" 3600 "$(fields 2 ieee1905.assoc_ctrl.validity)"
check "2 STA count" 2 "$(fields 2 ieee1905.assoc_ctrl.sta_list_count)"
check "2 both clients" aa:bb:cc:dd:ee:ff,0c:0e:5a:1b:2c:44 "$(fields 2 ieee1905.assoc_ctrl.target_mac_addr)"

# --- frame 3: 0x8015 Client Steering BTM Report -------------------------------------------
check "3 is a BTM report" 0x8015 "$(fields 3 ieee1905.message_type)"
check "3 client" aa:bb:cc:dd:ee:ff "$(fields 3 ieee1905.btm_report.mac_addr)"
check "3 status" 1 "$(fields 3 ieee1905.btm_report.status)"
check "3 target BSSID" 0c0e5a1b2c44 "$(fields 3 ieee1905.btm_report.target_bssid)"

# Nothing the dissector calls malformed, and no TLV carrying bytes it could not place: both mean our length
# fields are wrong, which the byte-literal test cannot tell us.
check "no malformed frame" "" "$("$TSHARK" -r "$T/steer.pcap" -Y '_ws.malformed || ieee1905.tlv.extra_data || ieee1905.tlv.too_short' -T fields -e frame.number 2>/dev/null)"

printf 'steer tshark: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ] || exit 1
exit 0
