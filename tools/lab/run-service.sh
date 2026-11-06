#!/bin/sh
# Run the controller service in the private sandbox on a lab host.
#
#   sudo sh tools/lab/run-service.sh --seconds 20
#   sudo sh tools/lab/run-service.sh --config /tmp/lab.conf --seconds 60
#   sudo sh tools/lab/run-service.sh --agent --seconds 60     # slave-side observation (M1/M2)
#
# Needs root: ns-run unshares the IPC/network/mount namespaces and bind-mounts the
# root read-only, then chroots and execs the factory loader. Everything lives in
# /tmp/zte-mesh-svc and /run/zte-mesh; nothing is installed.
#
# --agent runs db-engine-probe --agent-startup instead of --controller-service: the daemon boots as
# MultiAPCfg Mode 1 / SetRoleEnable 0 / EnMasterOnly 0 and this side plays the synthetic controller.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${OUT:-"$REPO/out"}
ROOT=/tmp/zte-mesh-svc/root
STATE=/run/zte-mesh
SECONDS_TO_RUN=0
CONFIG=
MODE=--controller-service

while [ $# -gt 0 ]; do
	case "$1" in
	--seconds) SECONDS_TO_RUN=$2; shift 2 ;;
	--config) CONFIG=$2; shift 2 ;;
	--agent) MODE=--agent-startup; shift ;;
	--no-rebuild) NO_REBUILD=1; shift ;;
	*) echo "usage: $0 [--seconds N] [--config FILE] [--agent] [--no-rebuild]" >&2; exit 2 ;;
	esac
done

[ "$(id -u)" = 0 ] || { echo "run me with sudo: ns-run needs to unshare namespaces" >&2; exit 1; }

if [ "${NO_REBUILD:-0}" != 1 ]; then
	sh "$REPO/tools/lab/mkroot.sh" "$ROOT" >/dev/null
fi

install -d -m 700 "$STATE"
if [ -n "$CONFIG" ]; then
	install -m 600 "$CONFIG" "$STATE/config"
else
	cat > "$STATE/config" <<'CONF'
version_mode=2
radio0.enabled=1
radio0.ssid=lab-2g
radio0.key=lab-key-24g-only
radio1.enabled=1
radio1.ssid=lab-5g
radio1.key=lab-key-5g-only
CONF
	chmod 600 "$STATE/config"
fi

: > /tmp/zte-mesh-svc/service.log
cd "$OUT"
# qemu-user needs more address space than the device's 128 MB limit allows; ns-run
# only relaxes it when this is set, and the device launcher never sets it.
ZTE_MESH_EMULATED_LAB=1
export ZTE_MESH_EMULATED_LAB
if [ "$SECONDS_TO_RUN" -gt 0 ]; then
	timeout "$SECONDS_TO_RUN" ./ns-run-svctest "$ROOT" /lib/ld-linux.so.3 --library-path /lib \
		/bin/db-engine-probe "$MODE" >> /tmp/zte-mesh-svc/service.log 2>&1 || true
else
	./ns-run-svctest "$ROOT" /lib/ld-linux.so.3 --library-path /lib \
		/bin/db-engine-probe "$MODE" >> /tmp/zte-mesh-svc/service.log 2>&1
fi

echo "--- service.log ---"
tail -n 40 /tmp/zte-mesh-svc/service.log
if [ -f "$STATE/state.json" ]; then
	echo "--- state.json ---"
	head -c 1200 "$STATE/state.json"; echo
fi
