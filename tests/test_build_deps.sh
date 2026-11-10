#!/bin/sh
# Host test for the "Decide runtime deps for this target" step in build-apk.yml, without a router or an SDK.
# The step trims runtime deps out of DEPENDS (so they never enter the build graph) and must move every trimmed
# dep into EXTRA_DEPENDS (so the apk still declares it). This extracts the step's own shell from the YAML and
# runs it against a copy of the package Makefile for all four switch combinations, then reads the two lines back.
# It then extracts the read-back step's python and feeds it adbdump fixtures, so the two halves are tested as
# one contract: what the trim moves into EXTRA_DEPENDS is what the artifact assertion demands.
#
#   sh tests/test_build_deps.sh [WORKFLOW] [MAKEFILE]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WF=${1:-$ROOT/.github/workflows/build-apk.yml}
MK_SRC=${2:-$ROOT/openwrt/zte-mesh/Makefile}
[ -f "$WF" ] || { echo "workflow not found: $WF" >&2; exit 1; }
[ -f "$MK_SRC" ] || { echo "Makefile not found: $MK_SRC" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT

# Pull the step's run block out of the YAML: everything between its "- name:" and the next one, with the
# 10-space block-scalar indent removed. Testing the extracted text (not a re-implementation) is the point --
# a sed that stops writing EXTRA_DEPENDS, or writes it unversioned, has to fail here.
awk '
  /^      - name: Decide runtime deps for this target$/ { grab = 1; next }
  grab && /^      - name: / { exit }
  grab && /^        run: \|$/ { inrun = 1; next }
  grab && inrun { sub(/^          /, ""); print }
' "$WF" > "$T/step.sh"
[ -s "$T/step.sh" ] || { echo "could not extract the runtime-deps step from $WF" >&2; exit 1; }

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}

# run_case TRIM KMOD -- lay out the relative paths the step expects, run it, echo nothing; sets CASE_RC.
run_case() {
	rm -rf "$T/tree"
	mkdir -p "$T/tree/sr1010/immortalwrt" "$T/tree/zte-mesh/openwrt/zte-mesh"
	cp "$MK_SRC" "$T/tree/zte-mesh/openwrt/zte-mesh/Makefile"
	CASE_RC=0
	( cd "$T/tree/sr1010/immortalwrt" &&
	  TRIM_IP_FULL="$1" TARGET_BUILDS_KERNEL_MODULES="$2" sh "$T/step.sh" ) >"$T/out" 2>&1 || CASE_RC=$?
	MK="$T/tree/zte-mesh/openwrt/zte-mesh/Makefile"
}

dep_line() { sed -n 's/^[[:space:]]*DEPENDS:=[[:space:]]*//p' "$MK" | head -n 1; }
extra_line() { sed -n 's/^[[:space:]]*EXTRA_DEPENDS:=//p' "$MK" | head -n 1; }
has_extra() { grep -qE '^[[:space:]]*EXTRA_DEPENDS:=' "$MK" && echo yes || echo no; }

# Default target: everything trimmed out of the graph, all three kept as runtime deps.
run_case true false
check "default rc" 0 "$CASE_RC"
check "default DEPENDS empty" "" "$(dep_line)"
check "default EXTRA present" yes "$(has_extra)"
check "default EXTRA value" "ip-full (>=0), kmod-veth (>=0), kmod-sched-core (>=0)" "$(extra_line)"

# In-tree kernel: the kmods stay in DEPENDS, only ip-full moves to EXTRA_DEPENDS.
run_case true true
check "kmod-build rc" 0 "$CASE_RC"
check "kmod-build DEPENDS" "+kmod-veth +kmod-sched-core" "$(dep_line)"
check "kmod-build EXTRA value" "ip-full (>=0)" "$(extra_line)"

# ip-full kept, kmods trimmed: only the kmods move to EXTRA_DEPENDS.
run_case false false
check "ip-kept rc" 0 "$CASE_RC"
check "ip-kept DEPENDS" "+ip-full" "$(dep_line)"
check "ip-kept EXTRA value" "kmod-veth (>=0), kmod-sched-core (>=0)" "$(extra_line)"

# Nothing trimmed: DEPENDS untouched and no EXTRA_DEPENDS line at all.
run_case false true
check "untouched rc" 0 "$CASE_RC"
check "untouched DEPENDS" "+ip-full +kmod-veth +kmod-sched-core" "$(dep_line)"
check "untouched EXTRA absent" no "$(has_extra)"

# The in-tree guard still fires: a Makefile that lost +kmod-veth must not pass when kmods are kept.
run_case false true
sed -i -E 's/[[:space:]]*\+kmod-veth//g' "$MK"
CASE_RC=0
( cd "$T/tree/sr1010/immortalwrt" &&
  TRIM_IP_FULL=false TARGET_BUILDS_KERNEL_MODULES=true sh "$T/step.sh" ) >"$T/out" 2>&1 || CASE_RC=$?
check "missing-kmod guard rc" 1 "$CASE_RC"

# Second half: trimming DEPENDS is only safe if EXTRA_DEPENDS carries the names into the finished apk, so
# the read-back step asks the artifact itself. Run 42343102511 went red on a correct apk because that
# reader walked every "name" in the adbdump document and collected the paths/files entries instead of
# info.depends, which is a list of plain strings. These fixtures pin the field it reads -- the last one
# puts the three names only in file names, the exact shape the broken reader accepted.
PY=$(command -v python3 || command -v python) || { echo "test_build_deps: no python3/python" >&2; exit 1; }
awk -v q="'" '
  /^      - name: Assert the zte-mesh apk carries the launcher for three architectures$/ { grab = 1; next }
  grab && /^      - name: / { exit }
  grab && index($0, "python3 -c " q) { inpy = 1; next }
  grab && inpy && $0 == "          " q " || exit 1" { exit }
  grab && inpy { sub(/^          /, ""); print }
' "$WF" > "$T/reader.py"
[ -s "$T/reader.py" ] || { echo "could not extract the apk depends reader from $WF" >&2; exit 1; }

fixture() { printf '%s\n' "$2" > "$T/$1.json"; }
read_case() { # NAME -- sets CASE_RC, leaves the reader's stdout+stderr in $T/reader-out
	CASE_RC=0
	"$PY" "$T/reader.py" < "$T/$1.json" >"$T/reader-out" 2>&1 || CASE_RC=$?
}
said() { grep -qxF "$1" "$T/reader-out" && echo yes || echo no; }

fixture real '{"info":{"name":"zte-mesh","version":"0.1.0-r24","depends":["ip-full>=0","kmod-sched-core>=0","kmod-veth>=0","libc"]},"paths":[{"name":"etc/config","acl":{"mode":493,"user":"root","group":"root"},"files":[{"name":"zte_mesh","size":8146,"hash":"6b6a048958197c7b074d9cf25e46bc0c098ff75a4fe8e9f21eb2ec4db642a9ca"}]}]}'
read_case real
check "real rc" 0 "$CASE_RC"
check "real echoes depends" yes "$(said 'apk depends: ip-full>=0, kmod-sched-core>=0, kmod-veth>=0, libc')"
check "real verdict" yes "$(said 'runtime deps present: ip-full, kmod-sched-core, kmod-veth')"

# EXTRA_DEPENDS stopped reaching the pack step: only the libc that DEPENDS always implied is left.
fixture libc-only '{"info":{"name":"zte-mesh","depends":["libc"]},"paths":[]}'
read_case libc-only
check "libc-only rc" 1 "$CASE_RC"
check "libc-only names all three" yes "$(said 'apk lost runtime deps (EXTRA_DEPENDS did not reach it): ip-full, kmod-sched-core, kmod-veth')"

fixture wrong-name '{"info":{"name":"zte-mesh-launcher","depends":["ip-full>=0","kmod-sched-core>=0","kmod-veth>=0","libc"]},"paths":[]}'
read_case wrong-name
check "wrong-name rc" 1 "$CASE_RC"

fixture no-depends '{"info":{"name":"zte-mesh"},"paths":[]}'
read_case no-depends
check "no-depends rc" 1 "$CASE_RC"

# adbdump prints the constraint only when the pack step wrote one; the names alone must still count.
fixture unversioned '{"info":{"name":"zte-mesh","depends":["ip-full","kmod-veth","kmod-sched-core","libc"]},"paths":[]}'
read_case unversioned
check "unversioned rc" 0 "$CASE_RC"

# adbdump writes a file's basename under the directory named by paths[].name, so a package shipping files
# called ip-full / kmod-veth / kmod-sched-core hands the name-walker exactly the three strings it wants.
fixture files-only '{"info":{"name":"zte-mesh","depends":["libc"]},"paths":[{"name":"usr/bin","acl":{"mode":493,"user":"root","group":"root"},"files":[{"name":"ip-full","size":1}]},{"name":"lib/modules/6.12.103","acl":{"mode":493,"user":"root","group":"root"},"files":[{"name":"kmod-veth","size":1},{"name":"kmod-sched-core","size":1}]}]}'
read_case files-only
check "files-only rc" 1 "$CASE_RC"

echo "test_build_deps: $pass passed, $fail failed"
[ "$fail" = 0 ]