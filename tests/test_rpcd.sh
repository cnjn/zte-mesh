#!/bin/sh
# Host test of the rpcd plugin openwrt/zte-mesh/files/luci.zte-mesh: a stub jsonfilter (python3) and a fake controller service
# that consumes request files and answers in state.json. Run: sh tests/test_rpcd.sh [PLUGIN]
PLUGIN=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/luci.zte-mesh}
T=$(mktemp -d) || exit 1
# Git Bash's mktemp hands back a Windows drive prefix (C:\...), which the delete helpers on that host
# refuse; going through pwd gives the POSIX form there and changes nothing on Linux.
T=$(cd "$T" && pwd)
trap 'kill $SVC 2>/dev/null; rm -rf "$T"' EXIT
export ZTE_MESH_RUNDIR=$T/run ZTE_MESH_ACTION_WAIT=3
mkdir -p "$T/run/actions"
# The stub is a python file run through a shell function of the sourcing shell: the container that runs make test-host mounts
# /tmp noexec, so a script under $T could not be executed directly.
cat > "$T/jf.py" <<'PY'
import json, re, sys
a = sys.argv[1:]
src = None
expr = None
while a:
    o = a.pop(0)
    if o == "-i": src = json.load(open(a.pop(0)))
    elif o == "-s": src = json.loads(a.pop(0))
    elif o == "-e": expr = a.pop(0)
m = re.fullmatch(r"@\.actions\[@\.id='([^']*)'\]\.status", expr)
if m:
    for x in src.get("actions", []):
        if x.get("id") == m.group(1): print(x["status"])
    sys.exit(0)
m = re.fullmatch(r"@\.(\w+)", expr)
if m and m.group(1) in src:
    v = src[m.group(1)]
    print("true" if v is True else "false" if v is False else v)
PY
export ZTE_JF="$T/jf.py" ZTE_PLUGIN="$PLUGIN"
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); else echo "FAIL $1: got '$2' want '$3'"; fail=$((fail + 1)); fi; }
call() { printf '%s\n' "$2" | sh -c 'jsonfilter() { python3 "$ZTE_JF" "$@"; }; m=$1; set -- call "$m"; . "$ZTE_PLUGIN"' _ "$1" | tr -d ' \n'; }

# no running service (no state.json): -5
check no-state "$(call reboot_node '{"mac":"d4:b7:09:0d:77:30"}')" '{"result":-5}'
# list declares the methods
check list "$(sh -c 'set -- list; . "$ZTE_PLUGIN"' | tr -d ' ')" '{"status":{},"restart":{},"reboot_node":{"mac":"str"},"light_node":{"mac":"str","on":true}}'

# fake service: answer status 0 for every request, remember the request text
echo '{"actions":[]}' > "$T/run/state.json"

# status names the role, so a child router shows up as one and a script does not have to read /etc/config
status_out=$(call status '{}')
case "$status_out" in
*'"role":"master"'*) pass=$((pass + 1)) ;;
*) echo "FAIL status-role: got '$status_out'"; fail=$((fail + 1)) ;;
esac

# status also reports what the child-side applier did last: it runs outside the sandbox, so the service cannot know.
printf 'epoch=1761740000\nresult=ok\nchanged=1\nbands=2g 5g\nmessage=\n' > "$T/run/wlan-sync.status"
status_out=$(call status '{}')
case "$status_out" in
*'"sync":{"epoch":1761740000,"result":"ok","bands":"2g5g"'*) pass=$((pass + 1)) ;;
*) echo "FAIL status-sync: got '$status_out'"; fail=$((fail + 1)) ;;
esac
rm -f "$T/run/wlan-sync.status"
status_out=$(call status '{}')
case "$status_out" in
*'"sync":null'*) pass=$((pass + 1)) ;;
*) echo "FAIL status-sync-none: got '$status_out'"; fail=$((fail + 1)) ;;
esac
( while :; do
	for f in "$T"/run/actions/*.act; do
		[ -e "$f" ] || continue
		id=$(basename "$f" .act); cat "$f" >> "$T/seen"; rm -f "$f"
		printf '{"actions":[{"id":"%s","action":"x","mac":"-","value":0,"status":0,"epoch":1}]}\n' "$id" > "$T/run/state.json.new"
		mv "$T/run/state.json.new" "$T/run/state.json"
	done
	sleep 0.2
  done ) &
SVC=$!
check reboot "$(call reboot_node '{"mac":"d4:b7:09:0d:77:30"}')" '{"result":0}'
check light-on "$(call light_node '{"mac":"D4:B7:09:0D:77:30","on":true}')" '{"result":0}'
check light-off "$(call light_node '{"mac":"d4:b7:09:0d:77:30","on":false}')" '{"result":0}'
check seen "$(cat "$T/seen" | tr '\n' '|')" 'reboot d4:b7:09:0d:77:30|light d4:b7:09:0d:77:30 1|light d4:b7:09:0d:77:30 0|'
# bad input never reaches the service
: > "$T/seen"
check bad-mac "$(call reboot_node '{"mac":"d4:b7:09:0d:77"}')" '{"result":-2}'
check inject-mac "$(call reboot_node '{"mac":"d4:b7:09:0d:77:30 extra"}')" '{"result":-2}'
check newline-mac "$(call light_node '{"mac":"d4:b7:09:0d:77:30\nreboot aa:aa:aa:aa:aa:aa","on":true}')" '{"result":-2}'
check bad-on "$(call light_node '{"mac":"d4:b7:09:0d:77:30","on":"maybe"}')" '{"result":-2}'
check no-on "$(call light_node '{"mac":"d4:b7:09:0d:77:30"}')" '{"result":-2}'
check nothing-written "$(cat "$T/seen" 2>/dev/null | tr '\n' '|')" ''
check leftovers "$(ls "$T/run/actions" | tr '\n' ' ')" ''
# a service that never answers: null after the wait
kill $SVC; wait $SVC 2>/dev/null; SVC=
check no-answer "$(call reboot_node '{"mac":"d4:b7:09:0d:77:30"}')" '{"result":null}'
# actions directory missing (service not started yet): -5
rm -rf "$T/run/actions"
check no-dir "$(call reboot_node '{"mac":"d4:b7:09:0d:77:30"}')" '{"result":-5}'
echo "rpcd test: $pass passed, $fail failed"
[ "$fail" = 0 ]
