#!/bin/sh
# launcher-arches.sh -- compile the zte-mesh launcher binaries for every CPU one apk can carry.
#
#   sh tools/build/launcher-arches.sh SRC OUT
#
# SRC holds ns-run.c, controller-net.c and enroller.c; the zte-mesh Makefile unpacks them out of the
# runtime tarball it installs, so the packaged code cannot drift from what the repository ships. OUT gets
# one directory per architecture built -- OUT/aarch64/{ns-run,mesh-search} and so on -- which the package
# installs under /usr/libexec/zte-mesh/arch/.
#
# Why a loop instead of one $(TARGET_CC) build: the package is arch:all, so its apk has to carry the code
# for every machine it claims to install on. Inside an OpenWrt build tree only the target's own toolchain
# exists, so the other architectures come from host cross compilers where they are installed
# (.github/workflows/build-apk.yml installs them; a feed build inside a source tree gets the target arch
# alone, which the dispatcher then says out loud on any other machine).
#
# What the caller can set:
#
#   ZTE_MESH_TARGET_ARCH / ZTE_MESH_TARGET_CC / ZTE_MESH_TARGET_CFLAGS
#       the architecture this OpenWrt toolchain builds for, and the compiler and flags for it. The
#       package Makefile passes $(TARGET_CC) and $(TARGET_CFLAGS) through and leaves the
#       architecture to this script, which reads it off the compiler (see -dumpmachine below).
#   ZTE_MESH_CC_AARCH64 / ZTE_MESH_CC_ARMV7 / ZTE_MESH_CC_X86_64
#       build this architecture with this compiler instead of searching for one.
#   ZTE_MESH_ARCHES
#       which architectures to build (default: aarch64 armv7 x86_64).
#   ZTE_MESH_REQUIRE_ARCHES
#       architectures whose absence fails the script rather than only being reported. CI sets this to
#       the full list, so a one-architecture apk cannot be published quietly.
#
# Every binary is built -static on purpose: the packaged launcher has to run whatever libc is on the
# machine (docs/LAB.md section 7 tells what a dynamic binary costs when the image's musl is a different
# release than the toolchain's), and a host cross build has no target rootfs to link against anyway.
# -U_FORTIFY_SOURCE is there because Ubuntu's gcc predefines it, and with -static an unchecked write()
# return in controller-net.c becomes a fortify warning the build should not have to explain.

set -u

SRC=${1:-}
OUT=${2:-}
if [ -z "$SRC" ] || [ -z "$OUT" ]; then
	printf 'usage: %s SRC OUT\n' "$0" >&2
	exit 2
fi
for f in ns-run.c controller-net.c enroller.c; do
	[ -r "$SRC/$f" ] || { printf 'launcher-arches: %s/%s is missing\n' "$SRC" "$f" >&2; exit 2; }
done

ARCHES=${ZTE_MESH_ARCHES:-aarch64 armv7 x86_64}
REQUIRE=${ZTE_MESH_REQUIRE_ARCHES:-}
TARGET_ARCH=${ZTE_MESH_TARGET_ARCH:-}
TARGET_CC=${ZTE_MESH_TARGET_CC:-}
TARGET_CFLAGS=${ZTE_MESH_TARGET_CFLAGS:-}

# Which CPU the caller's toolchain emits, when the caller did not say. -dumpmachine rather than a
# Makefile variable: the directory names here have to match the compiler that produced them, and
# only the compiler knows what it targets. An unmapped triple (mips, riscv, ...) means the
# toolchain's own output is not something this package carries, so it is simply not installed.
if [ -z "$TARGET_ARCH" ] && [ -n "$TARGET_CC" ]; then
	# shellcheck disable=SC2086 -- the compiler command is a word list
	dump=$($TARGET_CC -dumpmachine 2>/dev/null)
	case $dump in
	aarch64*) TARGET_ARCH=aarch64 ;;
	arm*) TARGET_ARCH=armv7 ;;
	x86_64*) TARGET_ARCH=x86_64 ;;
	esac
	if [ -n "$TARGET_ARCH" ]; then
		printf 'target toolchain builds for %s\n' "$TARGET_ARCH"
	else
		printf 'target toolchain (%s) is not one of the architectures this package carries\n' \
			"${dump:-$TARGET_CC did not answer -dumpmachine}"
	fi
fi

# The same flags for every architecture; -static is not something a caller gets to drop.
FLAGS="-O2 -std=c11 -static -Wall -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
EXTRA_FLAGS=""

# The kernel UAPI headers ns-run.c and controller-net.c include (<linux/capability.h>, <linux/if_link.h>)
# sit in the host compiler's own include directories, which the host gcc searches but musl-gcc does not:
# its specs compile with -nostdinc. Ask the host compiler -- the one the fallback below is built with, so
# its answer is by definition enough for these sources -- which directories it would search, keep the ones
# actually carrying those headers, and hand them to the musl build with -idirafter so they land AFTER musl's
# libc headers: with -isystem they win over musl and the build dies inside glibc's errno.h instead.
uapi_flags=""
for dir in $(echo | cc -E -Wp,-v -x c - 2>&1 >/dev/null | sed -n '/^ /p' | tr -d ' '); do
	[ -f "$dir/linux/if_link.h" ] || [ -f "$dir/asm/types.h" ] || continue
	uapi_flags="$uapi_flags -idirafter $dir"
done

mkdir -p "$OUT" || exit 1

# try_cc ARCH SYSROOT COMPILER... -- one build of both programs. SYSROOT is "none" for the compiler's own
# search path; what follows it is the compiler, which may be several words.
try_cc() {
	target=$1
	sr=$2
	shift 2
	srarg=""
	[ "$sr" = none ] || srarg="--sysroot=$sr"
	# shellcheck disable=SC2086 -- FLAGS, EXTRA_FLAGS and the compiler command are word lists on purpose
	"$@" $FLAGS $srarg $EXTRA_FLAGS -o "$OUT/$target/ns-run" "$SRC/ns-run.c" "$SRC/controller-net.c" 2>"$OUT/.last-attempt.log" &&
		"$@" $FLAGS $srarg $EXTRA_FLAGS -o "$OUT/$target/mesh-search" "$SRC/enroller.c" 2>>"$OUT/.last-attempt.log"
}

built=""
for arch in $ARCHES; do
	case " aarch64 armv7 x86_64 " in
	*" $arch "*) ;;
	*) printf 'launcher-arches: %s is not an architecture this package carries\n' "$arch" >&2; exit 2 ;;
	esac
	mkdir -p "$OUT/$arch" || exit 1

	# Candidates, in order. Debian's cross gcc sometimes searches the host /usr/include first for a
	# foreign target ("bits/wordsize.h not found"), so the explicit sysroots are tried before the
	# default one -- the same order and the same compilers .github/workflows/build-lab-tools.yml uses.
	# armv7 takes the hard-float compiler first and the soft-float one as a backup: a -static binary
	# carries its own libc, so the float ABI is nobody's business at run time, and an armel toolchain
	# still produces something an armv7 machine can run.
	override=""
	case $arch in
	aarch64) override=${ZTE_MESH_CC_AARCH64:-} ;;
	armv7) override=${ZTE_MESH_CC_ARMV7:-} ;;
	x86_64) override=${ZTE_MESH_CC_X86_64:-} ;;
	esac
	if [ "$arch" = "$TARGET_ARCH" ] && [ -n "$TARGET_CC" ]; then
		EXTRA_FLAGS="$TARGET_CFLAGS"
		candidates=$TARGET_CC
		sysroots=none
	else
		EXTRA_FLAGS=""
		candidates=$override
		[ -n "$candidates" ] || case $arch in
			aarch64) candidates="aarch64-linux-gnu-gcc" ;;
			armv7) candidates="arm-linux-gnueabihf-gcc arm-linux-gnueabi-gcc" ;;
			*) candidates="musl-gcc cc" ;;
		esac
		sysroots=none
		[ -n "$override" ] || case $arch in
			aarch64) sysroots="/ /usr/aarch64-linux-gnu none" ;;
			armv7) sysroots="/ /usr/arm-linux-gnueabihf none" ;;
		esac
	fi

	ok=""
	attempted=""
	for cc in $candidates; do
		command -v "$cc" >/dev/null 2>&1 || continue
		attempted=1
		# The target's own toolchain already has the kernel headers in its sysroot, so only the host
		# cross builds above get to need them. The match has to be after -musl-gcc too: OpenWrt's own
		# aarch64 compiler is named ...-linux-musl-gcc and must keep $(TARGET_CFLAGS), not these flags.
		if [ "$arch" != "$TARGET_ARCH" ]; then
			case $cc in
			*-musl-gcc | musl-gcc) EXTRA_FLAGS=$uapi_flags ;;
			*) EXTRA_FLAGS="" ;;
			esac
		fi
		for sr in $sysroots; do
			# shellcheck disable=SC2086 -- the compiler may be several words
			try_cc "$arch" "$sr" $cc && {
				label=$cc
				[ "$sr" = none ] || label="$cc --sysroot=$sr"
				printf 'built %s with %s\n' "$arch" "$label"
				ok=1
				break
			}
		done
		[ -n "$ok" ] && break
	done

	if [ -n "$ok" ]; then
		built="$built $arch"
	else
		if [ -z "$attempted" ]; then
			printf 'no compiler for %s (looked for: %s) -- machines running %s get no launcher from this package\n' \
				"$arch" "$candidates" "$arch" >&2
		else
			printf 'every compiler for %s failed (tried: %s); the last one said:\n' "$arch" "$candidates" >&2
			tail -n 5 "$OUT/.last-attempt.log" 2>/dev/null >&2
		fi
		rm -f "$OUT/$arch/ns-run" "$OUT/$arch/mesh-search"
		rmdir "$OUT/$arch" 2>/dev/null
	fi
done

rm -f "$OUT/.last-attempt.log"
printf 'architectures built:%s\n' "$built"

if [ -z "$built" ]; then
	printf 'launcher-arches: nothing built -- there would be no launcher in this package\n' >&2
	exit 1
fi
for want in $REQUIRE; do
	case " $built " in
	*" $want "*) ;;
	*)
		printf 'launcher-arches: %s was required and is not in the package\n' "$want" >&2
		exit 1
		;;
	esac
done
exit 0
