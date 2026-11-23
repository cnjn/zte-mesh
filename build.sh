#!/bin/sh
# Build/audit/probe only. Device deployment is a separate explicit command.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${OUT:-"$SCRIPT_DIR/out"}
IMAGE=${IMAGE:-sr1010-mesh-shim-builder}
export PYTHONDONTWRITEBYTECODE=1

for tool in python3 docker unsquashfs; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'missing tool: %s\n' "$tool" >&2; exit 1;
    }
done
python3 "$SCRIPT_DIR/tools/dev/prepare.py" --output "$OUT" --analysis-cspd
python3 "$SCRIPT_DIR/tools/dev/schema.py" --verify-cspd "$OUT/analysis/cspd" \
    --verify-libdb "$OUT/runtime/lib/libdb.so" --output "$OUT/multiap-cfg-schema.json" \
    --policies-output "$OUT/mesh-policy-schema.json"
python3 "$SCRIPT_DIR/tools/dev/profile_schema.py" --verify-cspd "$OUT/analysis/cspd" \
    --verify-libdb "$OUT/runtime/lib/libdb.so" --output "$OUT/wlan-profile-schema.json"
python3 "$SCRIPT_DIR/tools/dev/bridge_evidence.py" --libcfapi "$OUT/runtime/lib/libcfapi.so" \
    --output "$OUT/bridge-security-map.json"
docker build --platform linux/arm64 -t "$IMAGE" "$SCRIPT_DIR"
docker image inspect --format '{{.Id}}' "$IMAGE" > "$OUT/builder-image.txt"
docker run --rm --platform linux/arm64 --network none --read-only \
    --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /tmp:rw,nosuid,size=64m --pids-limit 64 --memory 512m \
    -e PYTHONDONTWRITEBYTECODE=1 \
    -v "$SCRIPT_DIR:/src:ro" -v "$OUT:/artifacts" "$IMAGE" \
    make ARTIFACTS=/artifacts all database controller test-host
docker run --rm --platform linux/arm64 --network none --read-only \
    --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /tmp:rw,nosuid,size=32m --pids-limit 64 --memory 256m \
    -e PYTHONDONTWRITEBYTECODE=1 \
    -v "$SCRIPT_DIR:/src:ro" -v "$OUT:/artifacts" "$IMAGE" \
    python3 tools/dev/smoke.py --output /artifacts --qemu /usr/bin/qemu-arm-static
