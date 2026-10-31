#!/bin/sh
# Pack the audited vendor runtime + launcher (on the mac, after build.sh or container-make.sh).
#   install  openwrt/zte-mesh/zte-mesh-runtime.tar.gz  for the OpenWrt package, committed with it
#            (ns-run for /usr/lib/zte-mesh/root)
#   svctest  out/zte-mesh-svctest.tar.gz  for the tmpfs lab tools (dev-*.py, tools/capture/mreplay.py)
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${OUT:-"$REPO/out"}
case "${1:-}" in
install)
	sh "$REPO/tools/build/container-make.sh" ns-run
	python3 "$REPO/openwrt/make-runtime.py" --output "$OUT" --ns-run "$OUT/ns-run" \
		--archive "$REPO/openwrt/zte-mesh/zte-mesh-runtime.tar.gz" \
		--extra "$REPO/ns-run.c" "$REPO/controller-net.c" "$REPO/enroller.c" ;;
svctest)
	sh "$REPO/tools/build/container-make.sh" ns-run-svctest
	python3 "$REPO/openwrt/make-runtime.py" --output "$OUT" --ns-run "$OUT/ns-run-svctest" \
		--archive "$OUT/zte-mesh-svctest.tar.gz" --extra "$REPO/openwrt/zte-mesh/files/attach" ;;
*)
	echo "usage: $0 install|svctest" >&2
	exit 2 ;;
esac
