#!/bin/sh
# Host test of openwrt/zte-mesh/files/launcher-dispatch: the entry script the package installs twice (as
# ns-run and as mesh-search), the machine it asks `uname -m` about, and the transparency of the exec it ends
# with.
#
# The binaries are stubs that print their own path plus the arguments they were handed, and `uname -m` is a
# stub that answers whatever the case says. That is the point: nothing here compiles anything, and what gets
# graded is the mapping from the machine to arch/<cpu>/<name>, which binary the name in $0 picks, and whether
# an answer a board cannot use turns into words instead of a respawn loop.
#
# Run with the shell OpenWrt uses: busybox ash tests/test_launcher_dispatch.sh
#
#   sh tests/test_launcher_dispatch.sh [LAUNCHER_DISPATCH]
DISPATCH=${1:-$(dirname "$0")/../openwrt/zte-mesh/files/launcher-dispatch}
[ -f "$DISPATCH" ] || { echo "launcher-dispatch not found: $DISPATCH" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
LIB=$T/libexec
BIN=$T/bin
mkdir -p "$LIB" "$BIN"

pass=0
fail=0
skipped=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}
has() { # NAME NEEDLE TEXT -- the words a board has to be able to read
	case "$3" in
	*"$2"*) pass=$((pass + 1)) ;;
	*)
		echo "FAIL $1: '$2' is not in '$3'"
		fail=$((fail + 1))
		;;
	esac
}

# The machine under test. `uname -m` is the only question the dispatcher asks the board it is on, so the
# stub answers that one and the case says what it should say.
cat > "$BIN/uname" <<'STUB'
#!/bin/sh
[ "$1" = -m ] && { printf '%s\n' "$ZTE_MESH_TEST_MACHINE"; exit 0; }
printf 'stub\n'
STUB

# What the stub binaries report: which architecture's file ran, which name it was installed under, and the
# arguments it got. The identity comes out of $0, so a case that names mesh-search cannot pass by accident
# because both slots come from the same generator.
cat > "$BIN/launcher-stub" <<'STUB'
#!/bin/sh
name=${0##*/}
parent=${0%/*}
printf 'ran %s/%s' "${parent##*/}" "$name"
for argument in "$@"; do printf ' [%s]' "$argument"; done
printf '\n'
exit ${ZTE_MESH_TEST_EXIT:-0}
STUB
chmod +x "$BIN"/*
PATH=$BIN:$PATH
export PATH

# on_machine NAME -- pretend the next call runs on this CPU.
on_machine() {
	ZTE_MESH_TEST_MACHINE=$1
	export ZTE_MESH_TEST_MACHINE
}

# install_tree ARCH... -- the package's own install step: the dispatcher under two names, plus one build per
# architecture this imaginary board was shipped with.
install_tree() {
	rm -rf "$LIB/arch"
	mkdir -p "$LIB/arch"
	for arch in "$@"; do
		mkdir -p "$LIB/arch/$arch"
		for program in ns-run mesh-search; do
			cp "$BIN/launcher-stub" "$LIB/arch/$arch/$program"
			chmod 755 "$LIB/arch/$arch/$program"
		done
	done
	cp "$DISPATCH" "$LIB/ns-run"
	chmod 755 "$LIB/ns-run"
	cp "$LIB/ns-run" "$LIB/mesh-search"
}

# run_as ENTRY ARGS... -- the installed entry point, the way procd calls it.
run_as() {
	entry=$1
	shift
	sh "$LIB/$entry" "$@"
}

install_tree aarch64 armv7 x86_64

# --- the machine picks the directory -----------------------------------------------------------------------------
on_machine x86_64
out=$(run_as ns-run /usr/lib/zte-mesh/root /lib/ld-linux.so.3 --controller-service 2>&1)
check "x86_64 runs its own build" "ran x86_64/ns-run [/usr/lib/zte-mesh/root] [/lib/ld-linux.so.3] [--controller-service]" "$out"

# The same file under the other name: one script, two programs, chosen by $0 -- which is how the package
# installs ns-run and mesh-search from a single file.
out=$(run_as mesh-search br-lan 300 --al-mac 02:00:00:00:19:06 --search 2>&1)
check "the name chooses the program" "ran x86_64/mesh-search [br-lan] [300] [--al-mac] [02:00:00:00:19:06] [--search]" "$out"

on_machine aarch64
check "aarch64" "ran aarch64/ns-run [x]" "$(run_as ns-run x 2>&1)"
on_machine arm64
check "arm64 is aarch64" "ran aarch64/ns-run [x]" "$(run_as ns-run x 2>&1)"
on_machine armv7l
check "armv7l is armv7" "ran armv7/ns-run [x]" "$(run_as ns-run x 2>&1)"

# An argument that is empty stays empty, and one with a space stays one argument: this script sits in the
# command line of every instance, so it may not reshape what procd passes to ns-run.
on_machine x86_64
check "arguments are passed through whole" "ran x86_64/ns-run [] [a b]" "$(run_as ns-run '' 'a b' 2>&1)"

# procd watches the instance's own pid, and attach looks for the daemon's command line -- both only work if
# this script is replaced by the binary rather than sitting in front of it. The stub exits 7 for this.
on_machine x86_64
ZTE_MESH_TEST_EXIT=7 sh "$LIB/ns-run" >/dev/null 2>&1
check "the exit status is the binary's" "7" "$?"

# The path has to come from $0, not from a hardcoded /usr/libexec: ZTE_MESH_ROOT and a test's temporary root
# both put it somewhere else.
install_tree x86_64
on_machine x86_64
out=$(cd "$LIB" && sh ./ns-run y 2>&1)
check "a relative call still finds arch/" "ran x86_64/ns-run [y]" "$out"

# --- a machine the package does not carry -----------------------------------------------------------------------
install_tree aarch64 armv7 x86_64
on_machine mips64
out=$(run_as ns-run x 2>&1)
rc=$?
check "an uncarried machine exits 127" "127" "$rc"
has "the message names the machine" "uname -m says mips64" "$out"
has "the message lists what is carried" "carries: aarch64 armv7 x86_64" "$out"

# The list comes out of the directories on the box, so a one-architecture build -- a feed compiled inside a
# source tree, where only that target's toolchain exists -- says what it actually carries.
install_tree x86_64
on_machine armv7l
has "one-arch package reports one arch" "carries: x86_64" "$(run_as mesh-search x 2>&1)"

# Nothing on the box at all: the entry script installed and no build beside it.
rm -rf "$LIB/arch"
on_machine x86_64
has "no arch directory says none" "carries: none" "$(run_as ns-run x 2>&1)"

# A file that is there but not runnable is not a launcher: the same answer as a missing one, rather than the
# shell's own permission error three seconds after procd started the instance.
install_tree x86_64
chmod 644 "$LIB/arch/x86_64/ns-run"
chmod 644 "$LIB/arch/x86_64/mesh-search"
on_machine x86_64
if [ -x "$LIB/arch/x86_64/ns-run" ]; then
	# Not every filesystem can express "present but not runnable": Windows keeps a chmod-644 file
	# executable, so the two cases below would be graded against a binary the platform still runs.
	# They count as skipped here and are what busybox ash on a router checks.
	skipped=$((skipped + 2))
	printf 'skip %s not-runnable cases: this filesystem keeps a 644 file executable\n' 2
else
	out=$(run_as ns-run x 2>&1)
	rc=$?
	check "not runnable exits 127" "127" "$rc"
	has "not runnable names the machine" "uname -m says x86_64" "$out"
	# the other sentence, because the two failures are fixed differently: here the build is on the disk and the
	# carried list would only send the reader to the wrong architecture
	has "not runnable says the build is there" "is not runnable on this machine" "$out"

	# A not-runnable build is a failure for the check too, or the service starts and respawns 127 for ever.
	"$LIB/mesh-search" --launcher-check >/dev/null 2>&1
	check "the check rejects a build it cannot exec" "127" "$?"
fi

# --- what zte-mesh.init asks before it starts an instance --------------------------------------------------------
install_tree aarch64 armv7 x86_64
on_machine armv7l
check "the check answers with the path" "$LIB/arch/armv7/ns-run" "$("$LIB/ns-run" --launcher-check 2>&1)"
check "the check knows the two names apart" "$LIB/arch/armv7/mesh-search" "$("$LIB/mesh-search" --launcher-check 2>&1)"

# It must not run the program it is asking about: the stub would print a line, and on a real board this is
# the difference between a check and a second controller.
on_machine x86_64
out=$("$LIB/ns-run" --launcher-check 2>&1)
rc=$?
check "the check prints the path it resolved" "$LIB/arch/x86_64/ns-run" "$out"
check "the check exits 0 on a carried machine" "0" "$rc"
case $out in
*ran\ x86_64*) fail=$((fail + 1)); echo "FAIL the check ran the launcher: $out" ;;
*) pass=$((pass + 1)) ;;
esac

on_machine mips64
"$LIB/ns-run" --launcher-check >/dev/null 2>&1
check "the check fails on an uncarried machine" "127" "$?"

# The answer has to be the sentence a person running the program would read, not a shorter one written for the
# check: zte-mesh.init logs it and tools/lab/verify-install.sh prints its first line as evidence, so an answer
# that only said "not runnable" would leave whoever reads it guessing which CPUs this apk holds.
out=$("$LIB/ns-run" --launcher-check 2>&1)
check "the check names the machine it is standing on" \
	"zte-mesh ns-run: no build for this machine (uname -m says mips64). The launcher on this box carries: aarch64 armv7 x86_64" \
	"$(printf '%s\n' "$out" | head -n 1)"

echo "launcher dispatch test: $pass passed, $fail failed${skipped:+, $skipped skipped}"
[ "$fail" = 0 ]
