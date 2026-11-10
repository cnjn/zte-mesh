#!/bin/sh
# Host test of tools/build/launcher-arches.sh: which compilers the multi-architecture launcher build tries,
# what it produces for each, and what it says when one of them is missing.
#
# No real compiler is called. $PATH gets stub compilers named after the triples they claim (--dumpmachine
# answers their own name), and they write a 20-byte ELF header where the -o names one -- honest ones for the CPU
# the output directory claims, `cc-liar` always for aarch64. That is enough to grade the part that matters for
# packaging: an apk is only "one package, any machine" if the build says out loud which machines it actually
# covers, checks that claim against the binary it produced, fails when CI asked for all three, and leaves no
# half-built directory behind.
#
# Run with the shell OpenWrt uses: busybox ash tests/test_launcher_arches.sh
#
#   sh tests/test_launcher_arches.sh [LAUNCHER_ARCHES]
ARCHES=${1:-$(dirname "$0")/../tools/build/launcher-arches.sh}
[ -f "$ARCHES" ] || { echo "launcher-arches.sh not found: $ARCHES" >&2; exit 1; }
T=$(mktemp -d) || exit 1
T=$(cd "$T" && pwd)
trap 'rm -rf "$T"' EXIT
SRC=$T/src
BIN=$T/bin
OUT=$T/out
LOG=$T/cc.log
mkdir -p "$SRC" "$BIN"
: > "$LOG"

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}
has() { # NAME NEEDLE TEXT
	case "$3" in
	*"$2"*) pass=$((pass + 1)) ;;
	*)
		echo "FAIL $1: '$2' is not in '$3'"
		fail=$((fail + 1))
		;;
	esac
}
present() { # FILE -- is this build output there?
	[ -f "$1" ] && printf yes || printf no
}

for f in ns-run.c controller-net.c enroller.c; do printf 'int main(void){return 0;}\n' > "$SRC/$f"; done

cat > "$BIN/stub" <<'STUB'
#!/bin/sh
# A compiler that only pretends: --dumpmachine answers the triple its own file name is built from, and a
# build is a 20-byte ELF header written at -o. "bad" in the name is the compiler that fails, "liar" the one
# that builds for the wrong CPU -- it always emits aarch64, which is what the host's own cc does on an ARM64
# build machine when the script asks it for x86_64.
me=${0##*/}
printf '%s\n' "$*" >> "$ZTE_MESH_TEST_CC_LOG"
triple=${me%-gcc}
[ "$triple" = "$me" ] && triple=""
[ "$1" = -dumpmachine ] && { printf '%s\n' "$triple"; exit 0; }
case $me in
*bad*)
	printf '%s: deliberate failure\n' "$me" >&2
	exit 1
	;;
esac
out=""
prev=""
for a in "$@"; do
	[ "$prev" = -o ] && out=$a
	prev=$a
done
[ -n "$out" ] || { printf '%s: no -o\n' "$me" >&2; exit 1; }
# The output directory is the CPU the build claims, so an honest stub says the same thing back.
arch=${out%/*}; arch=${arch##*/}
case $me in
*liar*) arch=aarch64 ;;
esac
case $arch in
aarch64) class=2; machine=183 ;;
armv7) class=1; machine=40 ;;
*) class=2; machine=62 ;;
esac
# awk's %c on a number is the byte, which printf's \0 escape is not: dash reads at most two octal digits
# after it, so \0177 comes out as byte 15 and a literal '7' -- a header that is not an ELF header at all.
LC_ALL=C awk -v class="$class" -v machine="$machine" 'BEGIN {
	printf "%c%c%c%c%c%c%c%c", 127, 69, 76, 70, class, 1, 1, 0
	for (i = 0; i < 8; i++) printf "%c", 0
	printf "%c%c%c%c", 2, 0, machine % 256, int(machine / 256)
}' > "$out"
chmod 755 "$out"
STUB

# One stub per role. The two named after triples are what $(TARGET_CC) looks like to the script; neither
# name is in the script's default candidate list, so a case that does not set a compiler really has none.
# cc-ok and cc-bad are the only names a case can point at through ZTE_MESH_CC_*.
for name in cc-ok cc-bad cc-liar aarch64-openwrt-linux-musl-gcc mipsel-openwrt-linux-gnu-gcc; do
	cp "$BIN/stub" "$BIN/$name"
	chmod 755 "$BIN/$name"
done
PATH=$BIN:$PATH
export PATH
ZTE_MESH_TEST_CC_LOG=$LOG
export ZTE_MESH_TEST_CC_LOG

# build VAR=VALUE... -- -- run the script with a clean knob set, keep stdout, stderr and the status.
build() {
	unset ZTE_MESH_ARCHES ZTE_MESH_REQUIRE_ARCHES ZTE_MESH_TARGET_ARCH ZTE_MESH_TARGET_CC ZTE_MESH_TARGET_CFLAGS
	unset ZTE_MESH_CC_AARCH64 ZTE_MESH_CC_ARMV7 ZTE_MESH_CC_X86_64
	while [ "$1" != '--' ]; do
		eval "export ${1%%=*}='${1#*=}'"
		shift
	done
	shift
	: > "$LOG"
	rm -rf "$OUT"
	mkdir -p "$OUT"
	out=$(sh "$ARCHES" "$SRC" "$OUT" 2>&1)
	rc=$?
}

build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7=cc-ok' 'ZTE_MESH_CC_X86_64=cc-ok' --
check "three overrides build three" "0" "$rc"
check "aarch64 has both programs" "yes yes" "$(present "$OUT/aarch64/ns-run") $(present "$OUT/aarch64/mesh-search")"
check "armv7 has both programs" "yes yes" "$(present "$OUT/armv7/ns-run") $(present "$OUT/armv7/mesh-search")"
check "x86_64 has both programs" "yes yes" "$(present "$OUT/x86_64/ns-run") $(present "$OUT/x86_64/mesh-search")"
has "the summary lists all three" "architectures built: aarch64 armv7 x86_64" "$out"

# -static is the promise that lets one binary serve every firmware release of the same CPU; a caller must
# not be able to lose it, so it is asserted on what the compiler actually received.
has "every build is static" "-static" "$(tr '\n' ' ' < "$LOG")"

# --- what a missing architecture costs ---------------------------------------------------------------------------
# A compiler name that cannot exist: the script's own default candidate lists are whatever the build host
# has, so every case names its compilers and this is how a case says "there is none for this CPU".
NONE=ztmesh-no-such-compiler

build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7='"$NONE" 'ZTE_MESH_CC_X86_64='"$NONE" --
check "a one-architecture package still builds" "0" "$rc"
has "and says which one it is" "architectures built: aarch64" "$out"
has "and warns about the rest" "machines running armv7 get no launcher" "$out"
check "no empty directory is left behind" "no" "$([ -d "$OUT/armv7" ] && printf yes || printf no)"

# CI asks for all three: publishing an apk that only covers one machine is the failure this script exists
# to make impossible.
build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7='"$NONE" 'ZTE_MESH_REQUIRE_ARCHES=armv7 aarch64' --
check "a required architecture fails the build" "1" "$rc"
has "and names it" "armv7 was required and is not in the package" "$out"

build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7=cc-bad cc-ok' 'ZTE_MESH_CC_X86_64='"$NONE" --
check "a candidate that fails is not the end" "0" "$rc"
has "the next candidate is tried" "built armv7 with cc-ok" "$out"

build 'ZTE_MESH_CC_AARCH64=cc-bad' 'ZTE_MESH_CC_ARMV7='"$NONE" 'ZTE_MESH_CC_X86_64='"$NONE" --
check "a compiler that fails is a failure" "1" "$rc"
has "and the log says which" "every compiler for aarch64 failed" "$out"
# the reason used to be redirected into the null that `2>/dev/null >&2` had just opened, so "the last one said:"
# was always followed by silence -- in the build log as well as here
has "and the compiler's own words reach the log" "deliberate failure" "$out"

# --- a compiler that answers for the wrong CPU ---------------------------------------------------------------------
# The x86_64 fallback is the build host's own musl-gcc/cc. On an ARM64 build machine that compiler emits ARM64
# code and exits 0, so the old script filed an ARM64 ELF under x86_64/ and reported success -- the dispatcher
# would then exec it on a real x86_64 router and fail there, with the package claiming to cover that CPU.
build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7=cc-ok' 'ZTE_MESH_CC_X86_64=cc-liar' --
check "the other two still build" "0" "$rc"
has "and are the ones listed" "architectures built: aarch64 armv7" "$out"
has "the wrong-CPU build is a failed attempt" "every compiler for x86_64 failed" "$out"
has "and the reason names the CPU" "is not x86_64 code" "$out"
check "no x86_64 binary is left behind" "no" "$(present "$OUT/x86_64/ns-run")"
check "no empty x86_64 directory either" "no" "$([ -d "$OUT/x86_64" ] && printf yes || printf no)"

build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7=cc-ok' 'ZTE_MESH_CC_X86_64=cc-liar' 'ZTE_MESH_REQUIRE_ARCHES=aarch64 armv7 x86_64' --
check "CI asking for all three refuses the impostor" "1" "$rc"
has "and names the architecture it did not get" "x86_64 was required and is not in the package" "$out"

# Rejection is per attempt: the next candidate still gets its turn, and an honest one wins.
build 'ZTE_MESH_CC_AARCH64=cc-ok' 'ZTE_MESH_CC_ARMV7=cc-liar cc-ok' 'ZTE_MESH_CC_X86_64=cc-ok' --
check "a wrong-CPU candidate does not end the search" "0" "$rc"
has "the honest candidate builds it" "built armv7 with cc-ok" "$out"
check "armv7 really is there" "yes" "$(present "$OUT/armv7/ns-run")"

# The target toolchain goes through the same read-back: a compiler named for one triple that emits another is
# not believed just because the caller passed it in.
build 'ZTE_MESH_TARGET_CC=cc-liar' 'ZTE_MESH_TARGET_ARCH=x86_64' 'ZTE_MESH_CC_AARCH64='"$NONE" 'ZTE_MESH_CC_ARMV7='"$NONE" --
check "the caller's own toolchain is checked too" "1" "$rc"
has "and nothing is reported as built" "architectures built:" "$out"
check "no x86_64 binary from the target toolchain" "no" "$(present "$OUT/x86_64/ns-run")"

# --- the toolchain the caller already has ------------------------------------------------------------------------
build 'ZTE_MESH_TARGET_CC=aarch64-openwrt-linux-musl-gcc' 'ZTE_MESH_TARGET_CFLAGS=-O2 -pipe -fno-caller-saves' 'ZTE_MESH_CC_ARMV7='"$NONE" 'ZTE_MESH_CC_X86_64='"$NONE" --
check "the target toolchain builds" "0" "$rc"
has "the architecture comes from -dumpmachine" "target toolchain builds for aarch64" "$out"
has "the target flags reach the compiler" "-fno-caller-saves" "$(tr '\n' ' ' < "$LOG")"
check "the target build lands in its directory" "yes" "$(present "$OUT/aarch64/ns-run")"

build 'ZTE_MESH_TARGET_CC=mipsel-openwrt-linux-gnu-gcc' 'ZTE_MESH_CC_AARCH64='"$NONE" 'ZTE_MESH_CC_ARMV7='"$NONE" 'ZTE_MESH_CC_X86_64='"$NONE" --
check "a target this package does not carry builds nothing" "1" "$rc"
has "and says so" "is not one of the architectures this package carries" "$out"
check "no directory for a cpu the package does not carry" "no" "$([ -d "$OUT/mipsel" ] && printf yes || printf no)"

# --- the caller's own mistakes -----------------------------------------------------------------------------------
build 'ZTE_MESH_ARCHES=mipsel' --
check "an architecture outside the package is refused" "2" "$rc"
rm -f "$SRC/enroller.c"
sh "$ARCHES" "$SRC" "$OUT" >/dev/null 2>&1
check "a missing source is refused before building" "2" "$?"
sh "$ARCHES" >/dev/null 2>&1
check "no arguments is a usage error" "2" "$?"

echo "launcher arches test: $pass passed, $fail failed"
[ "$fail" = 0 ]
