#!/bin/sh
# Assemble the tmpfs lab root that ns-run-svctest chroots into.
#
#   sh tools/lab/mkroot.sh [ROOT]           # default /tmp/zte-mesh-svc/root
#
# This is the same layout openwrt/make-runtime.py produces, built straight from a
# compiled out/. It exists because make-runtime.py calls prepare.verify_existing(),
# which refuses an out/ derived from the committed runtime bundle instead of the
# factory JFFS2 (see tools/lab/bootstrap-out.py) — correct for a package that ships
# to a router, unnecessary for a lab root under /tmp.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${OUT:-"$REPO/out"}
ROOT=${1:-/tmp/zte-mesh-svc/root}

for f in "$OUT/db-engine-probe" "$OUT/zte-meshctl" "$OUT/runtime/bin/multiapd" "$OUT/runtime/lib/ld-linux.so.3" \
         "$OUT/runtime/lib/libzte_mesh_shim.so" "$OUT/runtime/lib/libzte_controller_trace.so" \
         "$OUT/runtime/lib/libzte_controller_route.so" "$OUT/runtime/lib/libzte_controller_profile.so" \
         "$OUT/runtime/lib/libzte_agent_mm.so"; do
	[ -f "$f" ] || { echo "missing $f — run sh tools/lab/bootstrap-out.py && sh tools/lab/build.sh all database '$(pwd)/out/runtime/lib/libzte_controller_*.so'" >&2; exit 1; }
done

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/lib" "$ROOT/tmp" "$ROOT/dev/shm" "$ROOT/var/run" "$ROOT/var/tmp" "$ROOT/run/zte-mesh"
chmod 1777 "$ROOT/tmp" "$ROOT/dev/shm" "$ROOT/var/tmp"

cp "$OUT/db-engine-probe" "$ROOT/bin/db-engine-probe"
cp "$OUT/zte-meshctl" "$ROOT/bin/zte-meshctl"
cp "$OUT/runtime/bin/multiapd" "$ROOT/bin/multiapd"
# The factory debug channel client, when it has been built: the agent observation uses it to raise the
# daemon's own log level (`sendcmd multiapd.acd.acd -l 8`) without a factory `sendcmd`.
if [ -f "$OUT/zte-debugcmd" ]; then
	cp "$OUT/zte-debugcmd" "$ROOT/bin/zte-debugcmd"
	chmod 755 "$ROOT/bin/zte-debugcmd"
fi
cp "$OUT"/runtime/lib/*.so* "$ROOT/lib/"
# The factory-style client library zte-meshctl links (diagget pattern); prepare.py keeps it out of the daemon's closure.
if [ -d "$OUT/runtime-extra/lib" ]; then
	cp "$OUT"/runtime-extra/lib/*.so* "$ROOT/lib/"
fi
chmod 755 "$ROOT/bin/db-engine-probe" "$ROOT/bin/zte-meshctl" "$ROOT/bin/multiapd" "$ROOT/lib/ld-linux.so.3"

# ns-run expects an empty regular file here and bind-mounts /dev/urandom over it.
: > "$ROOT/dev/urandom"

echo "lab root ready: $ROOT"
ls "$ROOT/bin"
