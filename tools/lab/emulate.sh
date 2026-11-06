#!/bin/sh
# Rebuild ns-run's sandbox step by step, so an emulated run can be traced.
#
#   sudo sh tools/lab/emulate.sh --seconds 20
#   sudo sh tools/lab/emulate.sh --seconds 20 --strace
#
# Why this exists: ns-run execs the guest loader through binfmt, which means the
# emulator gets no flags — no `-strace`, no `-d`. Doing the namespace/mount/chroot
# work here and invoking qemu-arm-static explicitly keeps the sandbox equivalent
# while making the run debuggable. The guest environment is the same list ns-run.c
# sets, passed with qemu's -E so the host loader never sees an ARM library.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${OUT:-"$REPO/out"}
ROOT=/tmp/zte-mesh-svc/root
STATE=/run/zte-mesh
SECONDS_TO_RUN=0
STRACE=0
PROGRAM=/bin/db-engine-probe
ARGS="--controller-service"

while [ $# -gt 0 ]; do
	case "$1" in
	--seconds) SECONDS_TO_RUN=$2; shift 2 ;;
	--strace) STRACE=1; shift ;;
	--program) PROGRAM=$2; shift 2 ;;
	--) shift; ARGS=$*; break ;;
	*) echo "usage: $0 [--seconds N] [--strace] [--program PATH] [-- ARGS...]" >&2; exit 2 ;;
	esac
done

[ "$(id -u)" = 0 ] || { echo "run me with sudo" >&2; exit 1; }
[ -d "$ROOT/bin" ] || { echo "no lab root; run sh tools/lab/mkroot.sh first" >&2; exit 1; }

cp /usr/bin/qemu-arm-static "$ROOT/bin/qemu-arm-static"
chmod 755 "$ROOT/bin/qemu-arm-static"
: > "$ROOT/dev/urandom"

EMULATOR_FLAGS=""
[ "$STRACE" = 1 ] && EMULATOR_FLAGS="-strace"

# The guest environment, exactly what ns-run.c builds (minus the ModelName opt-in).
GUEST_ENV="-E ZTE_MESH_DB_SANDBOX=1 -E LD_BIND_NOW=1 -E LD_PRELOAD=/lib/libzte_mesh_shim.so \
-E ZTE_MESH_SHIM_MODE=trace -E ZTE_MESH_SHIM_ALLOW_TRACE=1 -E ZTE_MESH_EMULATED_LAB=1"

cat <<SCRIPT > /tmp/zte-mesh-svc/inner.sh
set -eu
mount -t tmpfs -o size=8m,mode=1777 tmpfs "$ROOT/tmp"
mount -t tmpfs -o size=1m,mode=1777 tmpfs "$ROOT/dev/shm"
mount --bind /dev/urandom "$ROOT/dev/urandom"
mount --bind "$STATE" "$ROOT/run/zte-mesh"
exec chroot "$ROOT" /bin/qemu-arm-static -L / $EMULATOR_FLAGS $GUEST_ENV \\
	/lib/ld-linux.so.3 --library-path /lib $PROGRAM $ARGS
SCRIPT

cd "$OUT"
if [ "$SECONDS_TO_RUN" -gt 0 ]; then
	timeout "$SECONDS_TO_RUN" unshare --ipc --net --mount --propagation private sh /tmp/zte-mesh-svc/inner.sh
else
	unshare --ipc --net --mount --propagation private sh /tmp/zte-mesh-svc/inner.sh
fi
