ARTIFACTS ?= out
ARMCC ?= arm-linux-gnueabi-gcc
HOSTCC ?= cc
# A runtime-only Ubuntu image ships libcrypto.so.3 without the -dev symlink, so the plain -lcrypto the
# host crypto codec wants cannot be resolved there; fall back to the exact soname in that case.
HOSTCRYPTO ?= $(shell printf 'int main(void){return 0;}\n' | $(HOSTCC) -x c - -lcrypto -o /dev/null 2>/dev/null \
                && echo -lcrypto || echo -l:libcrypto.so.3)
# Programs that run on the router (AArch64). Cross builds on non-ARM hosts pass ROUTERCC.
ROUTERCC ?= $(HOSTCC)
COMMON = -O2 -std=c11 -Wall -Wextra -Werror -fPIC -fvisibility=hidden \
         -fno-unwind-tables -fno-asynchronous-unwind-tables
LIBDIR = $(abspath $(ARTIFACTS)/runtime/lib)
# The factory DSOs rely on symbols supplied by other direct multiapd deps,
# not solely their own DT_NEEDED chains. Keep the audited closure in scope.
VENDOR_LIBS = $(shell python3 -c 'import json; m=json.load(open("$(ARTIFACTS)/manifest.json")); print(" ".join("-l:"+n[4:] for n in m["files"] if n.startswith("lib/")))')

.PHONY: all test-host database controller
controller: database $(LIBDIR)/libzte_controller_trace.so $(LIBDIR)/libzte_controller_route.so $(LIBDIR)/libzte_controller_profile.so $(LIBDIR)/libzte_agent_mm.so
	python3 tools/dev/cm-profile-evidence.py --libcmapi $(LIBDIR)/libcmapi.so --multiapd $(ARTIFACTS)/runtime/bin/multiapd --verify src/cm-profile-layout.inc
	python3 tools/dev/startup-schema.py --cspd $(ARTIFACTS)/analysis/cspd --libdb $(LIBDIR)/libdb.so --verify src/startup-views.inc
all: $(LIBDIR)/libzte_mesh_shim.so $(ARTIFACTS)/exec-check $(ARTIFACTS)/qemu-arm-static

# Factory-style OSS client (diagget pattern): liboss_pub, not the daemon's liboss.
CLIENT_LIBS = -l:liboss_pub.so -l:libdl.so.2 -l:libcommfun.so -l:libm.so.6 -l:libzte_slibc.so -l:libdb.so -l:libcfapi.so \
              -l:libcapability.so -l:libieee1905cfapi.so -l:libmultiapcfapi.so -l:libdhcp4scfapi.so -l:libdhcp4ccfapi.so \
              -l:libmultiapcmapi.so -l:libieee1905cmapi.so -l:libcmapi.so -l:libdhcp4scmapi.so -l:libdhcp4ccmapi.so -l:libpthread.so.0 -l:libc.so.6 -l:ld-linux.so.3 -l:libgcc_s.so.1

$(ARTIFACTS)/zte-meshctl: src/zte-meshctl.c Makefile $(ARTIFACTS)/manifest.json
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -no-pie -nostdlib \
	  $(shell $(ARMCC) -print-file-name=crt1.o) $(shell $(ARMCC) -print-file-name=crti.o) \
	  $(shell $(ARMCC) -print-file-name=crtbegin.o) src/zte-meshctl.c \
	  -L$(LIBDIR) -L$(abspath $(ARTIFACTS))/runtime-extra/lib -Wl,-rpath-link,$(LIBDIR) -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
	  -Wl,--no-as-needed,--allow-shlib-undefined,--start-group $(CLIENT_LIBS) -lgcc -Wl,--end-group \
	  $(shell $(ARMCC) -print-file-name=crtend.o) $(shell $(ARMCC) -print-file-name=crtn.o) -o $@

database: $(ARTIFACTS)/db-engine-probe $(ARTIFACTS)/zte-meshctl $(ARTIFACTS)/ns-run $(ARTIFACTS)/config-bridge-probe

$(ARTIFACTS)/db-engine-probe: src/db-engine-probe.c src/controller-run.c src/controller-service.c src/controller-service.h src/controller-packet.c src/controller-wsc.c src/controller-wsc-crypto.c src/controller-wsc-crypto.h src/controller-packet.h src/startup-views.inc src/multiap-cfg.c src/multiap-cfg.h src/multiap_cfg.def \
                           src/controller-1905.c src/controller-1905.h src/controller-steer.c src/controller-steer.h src/controller-actions.c src/controller-actions.h \
                           src/controller-peer.c src/controller-peer.h \
                           src/agent-sync.c src/agent-sync.h src/wlan_cfg.def src/wlan_psk.def \
                           src/mesh-policies.c src/mesh-policies.h src/multiap_master.def src/multiap_domain.def src/vendor-db.h \
                           src/wlan-profiles.c src/wlan-profiles.h src/wlan_base.def src/wlan_cfg.def src/wlan_ssid.def \
                           src/wlan_psk.def src/dev_info.def src/mesh-bridge.c src/mesh-bridge.h Makefile $(ARTIFACTS)/manifest.json
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -no-pie -nostdlib \
	  $(shell $(ARMCC) -print-file-name=crt1.o) $(shell $(ARMCC) -print-file-name=crti.o) \
	  $(shell $(ARMCC) -print-file-name=crtbegin.o) src/db-engine-probe.c src/controller-run.c src/controller-service.c src/controller-packet.c src/controller-1905.c src/controller-steer.c src/controller-peer.c src/controller-actions.c src/controller-wsc.c src/controller-wsc-crypto.c src/agent-sync.c src/multiap-cfg.c src/mesh-policies.c src/wlan-profiles.c src/mesh-bridge.c \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
	  -Wl,--no-as-needed,--start-group $(VENDOR_LIBS) -lgcc -Wl,--end-group \
	  $(shell $(ARMCC) -print-file-name=crtend.o) $(shell $(ARMCC) -print-file-name=crtn.o) -o $@

$(ARTIFACTS)/ns-run: src/ns-run.c src/controller-net.c
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror $(NSRUN_FLAGS) src/ns-run.c src/controller-net.c -o $@

# Static ARM32 client for the factory daemons' sendcmd debug channel (a lab tool, not part of the runtime).
$(ARTIFACTS)/zte-debugcmd: src/zte-debugcmd.c
	$(ARMCC) -O2 -std=gnu11 -Wall -Wextra -Werror -march=armv7-a -mfloat-abi=soft -static src/zte-debugcmd.c -o $@

# The same launcher for tmpfs lab runs (tools/dev/dev-service-smoke.py and friends unpack the runtime to /tmp/zte-mesh-svc).
$(ARTIFACTS)/ns-run-svctest: src/ns-run.c src/controller-net.c
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror -DSERVICE_ROOT='"/tmp/zte-mesh-svc/root"' src/ns-run.c src/controller-net.c -o $@

$(ARTIFACTS)/config-bridge-probe: src/config-bridge-probe.c src/mesh-bridge.c src/mesh-bridge.h
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror src/config-bridge-probe.c src/mesh-bridge.c -o $@

$(LIBDIR)/libzte_mesh_shim.so: src/shim.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_mesh_shim.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(LIBDIR)/libzte_controller_profile.so: src/controller-profile.c src/cm-profile-layout.inc src/vendor-db.h src/mesh-bridge.h src/wlan_cfg.def src/wlan_psk.def
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_profile.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) src/controller-profile.c \
	  -Wl,--no-as-needed,--start-group $(VENDOR_LIBS) -lgcc -Wl,--end-group -o $@

$(LIBDIR)/libzte_controller_route.so: src/controller-route.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_route.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(LIBDIR)/libzte_controller_trace.so: src/controller-trace.c src/controller-log.c src/controller-log.h src/controller-peer.c src/controller-peer.h src/controller-1905.c src/controller-1905.h src/controller-steer.c src/controller-steer.h src/agent-ap-metrics.c src/agent-ap-metrics.h
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_trace.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) src/controller-trace.c src/controller-log.c src/controller-peer.c src/controller-1905.c src/controller-steer.c src/agent-ap-metrics.c \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

# The child-side configuration-manager stand-in. Only the two WLAN setters are interposed, and each of them only
# reports success when the database row already holds the value the caller passed in (src/agent-mm.c).
$(LIBDIR)/libzte_agent_mm.so: src/agent-mm.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_agent_mm.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(ARTIFACTS)/exec-check: src/exec-check.c
	$(ROUTERCC) -O2 -static -Wall -Wextra -Werror $< -o $@

$(ARTIFACTS)/qemu-arm-static:
	cp /usr/bin/qemu-arm-static $@
	chmod 700 $@

$(ARTIFACTS)/tests:
	mkdir -p $@

$(ARTIFACTS)/tests/libmockdb.so: tests/mockdb.c | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -Wall -Wextra -Werror -fPIC -shared $< -o $@

$(ARTIFACTS)/tests/libshim-host.so: src/shim.c | $(ARTIFACTS)/tests
	$(HOSTCC) $(COMMON) -DZTE_MESH_HOST_TEST -shared -Wl,-z,defs $< -ldl -o $@

$(ARTIFACTS)/tests/caller: tests/caller.c $(ARTIFACTS)/tests/libmockdb.so
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -L$(ARTIFACTS)/tests \
	  -Wl,-rpath,'$$ORIGIN' -lmockdb -o $@

$(ARTIFACTS)/tests/no-backend: tests/no-backend.c | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -ldl -o $@

$(ARTIFACTS)/tests/bridge-codec: tests/bridge-codec.c src/mesh-bridge.c src/mesh-bridge.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/bridge-codec.c src/mesh-bridge.c -o $@

$(ARTIFACTS)/tests/cm-profile-codec: tests/cm-profile-codec.c src/controller-profile.c src/cm-profile-layout.inc src/wlan_cfg.def src/wlan_psk.def src/vendor-db.h src/mesh-bridge.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/cm-profile-codec.c src/controller-profile.c -o $@

# The packet builder also emits the two steering messages, so the steer encoder and the 1905 walk it rides on
# have to be in the link - they were missing here, and the test only failed to build on a host that rebuilt it.
$(ARTIFACTS)/tests/controller-packet-codec: tests/controller-packet-codec.c src/controller-packet.c src/controller-packet.h src/controller-steer.c src/controller-steer.h src/controller-1905.c src/controller-1905.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-packet-codec.c src/controller-packet.c src/controller-steer.c src/controller-1905.c -o $@

# The 1905 decoder: pure functions over one frame, so it runs without a namespace or a vendor library.
$(ARTIFACTS)/tests/controller-1905-codec: tests/controller-1905-codec.c src/controller-1905.c src/controller-1905.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-1905-codec.c src/controller-1905.c -o $@

# The steering messages: encoders plus the two reports an agent sends back. Same pure-function rule, and it
# reuses the 1905 header/TLV walk, so both objects go in.
$(ARTIFACTS)/tests/controller-steer-codec: tests/controller-steer-codec.c src/controller-steer.c src/controller-steer.h src/controller-1905.c src/controller-1905.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-steer-codec.c src/controller-steer.c src/controller-1905.c -o $@

# What one frame leaves in peers/: the per-node files the topology page reads. The directory is an environment
# variable, so two constructed frames from two MACs can be checked against a real temporary directory.
$(ARTIFACTS)/tests/controller-peer-codec: tests/controller-peer-codec.c src/controller-peer.c src/controller-peer.h src/controller-1905.c src/controller-1905.h src/controller-steer.c src/controller-steer.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-peer-codec.c src/controller-peer.c src/controller-1905.c src/controller-steer.c -o $@

# The client action state machine: what to send, when to repeat it, when to give up. Pure timing logic, driven
# by a made-up clock, so an hour of behaviour costs nothing to test.
$(ARTIFACTS)/tests/controller-actions-test: tests/controller-actions-test.c src/controller-actions.c src/controller-actions.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-actions-test.c src/controller-actions.c -o $@

# How much of the daemon's log stream reaches the device log. Same rule as the two above: the decision is a
# function of the format text and a clock the caller passes, so a whole configuration push - 1012 lines of it
# on a real child router - costs one loop to test.
$(ARTIFACTS)/tests/controller-log-codec: tests/controller-log-codec.c src/controller-log.c src/controller-log.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-log-codec.c src/controller-log.c -o $@

$(ARTIFACTS)/tests/controller-wsc-codec: tests/controller-wsc-codec.c src/controller-wsc.c src/controller-packet.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	  tests/controller-wsc-codec.c src/controller-wsc.c -Wl,--gc-sections -o $@

# The 0x1018 codec needs the crypto unit, so it links OpenSSL rather than the structure-only parser above.
$(ARTIFACTS)/tests/controller-wsc-crypto-codec: tests/controller-wsc-crypto-codec.c src/controller-wsc-crypto.c src/controller-wsc-crypto.h src/controller-packet.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	  tests/controller-wsc-crypto-codec.c src/controller-wsc-crypto.c -Wl,--gc-sections $(HOSTCRYPTO) -o $@

# The agent's station-count stand-in: the query it recognises, the table it reads and the frame it writes are all
# functions of bytes and one file, so the master's own reader can be pointed at the answer it produces.
$(ARTIFACTS)/tests/agent-ap-metrics-codec: tests/agent-ap-metrics-codec.c src/agent-ap-metrics.c src/agent-ap-metrics.h src/controller-1905.c src/controller-1905.h src/controller-peer.c src/controller-peer.h src/controller-steer.c src/controller-steer.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/agent-ap-metrics-codec.c src/agent-ap-metrics.c \
	  src/controller-1905.c src/controller-peer.c src/controller-steer.c -o $@

# The agent synchroniser's pure part (plan text + change detection); the vendor symbols are stubbed in the test.
$(ARTIFACTS)/tests/agent-sync-codec: tests/agent-sync-codec.c src/agent-sync.c src/agent-sync.h src/wlan_cfg.def src/wlan_psk.def | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -Isrc tests/agent-sync-codec.c src/agent-sync.c -o $@

test-host: $(ARTIFACTS)/tests/caller $(ARTIFACTS)/tests/no-backend $(ARTIFACTS)/tests/libshim-host.so $(ARTIFACTS)/ns-run $(ARTIFACTS)/tests/bridge-codec $(ARTIFACTS)/tests/cm-profile-codec $(ARTIFACTS)/tests/controller-packet-codec $(ARTIFACTS)/tests/controller-1905-codec $(ARTIFACTS)/tests/controller-steer-codec $(ARTIFACTS)/tests/controller-peer-codec $(ARTIFACTS)/tests/controller-log-codec $(ARTIFACTS)/tests/controller-actions-test $(ARTIFACTS)/tests/controller-wsc-codec $(ARTIFACTS)/tests/controller-wsc-crypto-codec $(ARTIFACTS)/tests/agent-ap-metrics-codec $(ARTIFACTS)/tests/agent-sync-codec
	$(ARTIFACTS)/tests/controller-wsc-codec
	$(ARTIFACTS)/tests/controller-wsc-crypto-codec
	$(ARTIFACTS)/tests/controller-packet-codec
	$(ARTIFACTS)/tests/controller-1905-codec
	$(ARTIFACTS)/tests/controller-steer-codec
	$(ARTIFACTS)/tests/controller-peer-codec
	$(ARTIFACTS)/tests/controller-log-codec
	$(ARTIFACTS)/tests/controller-actions-test
	sh tests/test_steer_tshark.sh $(abspath $(ARTIFACTS))/tests/controller-steer-codec
	$(ARTIFACTS)/tests/agent-ap-metrics-codec
	$(ARTIFACTS)/tests/agent-sync-codec
	$(ARTIFACTS)/tests/cm-profile-codec
	$(ARTIFACTS)/tests/bridge-codec
	python3 -m unittest discover -s tests -p test_evidence.py -v
	python3 -m unittest discover -s tests -p test_device_workspace.py -v
	python3 tests/test_schema.py $(abspath $(ARTIFACTS))
	python3 tests/test_policies.py $(abspath $(ARTIFACTS))
	python3 tests/test_profiles.py $(abspath $(ARTIFACTS))
	python3 tests/test_startup.py $(abspath $(ARTIFACTS))
	python3 tests/test_cmapi_tables.py $(abspath $(ARTIFACTS))
	sh tests/test_init.sh
	sh tests/test_rpcd.sh
	sh tests/test_wlan_sync.sh
	sh tests/test_sta_collect.sh
	sh tests/test_launcher_dispatch.sh
	sh tests/test_launcher_arches.sh
	sh tests/test_build_deps.sh
	sh tests/test_service_role.sh
	sh tests/test_verify_install.sh
	python3 tests/test_i18n.py
	python3 tests/test_runtime.py
	python3 tests/test_frames.py
	@if command -v node >/dev/null 2>&1; then node tests/test_luci.js; else echo "test_luci: skipped (node is not installed)"; fi
	python3 tests/test_preload.py $(abspath $(ARTIFACTS)/tests)
	python3 tests/test_sandbox.py $(abspath $(ARTIFACTS)/ns-run)
