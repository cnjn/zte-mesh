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
	python3 cm-profile-evidence.py --libcmapi $(LIBDIR)/libcmapi.so --multiapd $(ARTIFACTS)/runtime/bin/multiapd --verify cm-profile-layout.inc
	python3 startup-schema.py --cspd $(ARTIFACTS)/analysis/cspd --libdb $(LIBDIR)/libdb.so --verify startup-views.inc
all: $(LIBDIR)/libzte_mesh_shim.so $(ARTIFACTS)/exec-check $(ARTIFACTS)/qemu-arm-static

# Factory-style OSS client (diagget pattern): liboss_pub, not the daemon's liboss.
CLIENT_LIBS = -l:liboss_pub.so -l:libdl.so.2 -l:libcommfun.so -l:libm.so.6 -l:libzte_slibc.so -l:libdb.so -l:libcfapi.so \
              -l:libcapability.so -l:libieee1905cfapi.so -l:libmultiapcfapi.so -l:libdhcp4scfapi.so -l:libdhcp4ccfapi.so \
              -l:libmultiapcmapi.so -l:libieee1905cmapi.so -l:libcmapi.so -l:libdhcp4scmapi.so -l:libdhcp4ccmapi.so -l:libpthread.so.0 -l:libc.so.6 -l:ld-linux.so.3 -l:libgcc_s.so.1

$(ARTIFACTS)/zte-meshctl: zte-meshctl.c Makefile $(ARTIFACTS)/manifest.json
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -no-pie -nostdlib \
	  $(shell $(ARMCC) -print-file-name=crt1.o) $(shell $(ARMCC) -print-file-name=crti.o) \
	  $(shell $(ARMCC) -print-file-name=crtbegin.o) zte-meshctl.c \
	  -L$(LIBDIR) -L$(abspath $(ARTIFACTS))/runtime-extra/lib -Wl,-rpath-link,$(LIBDIR) -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
	  -Wl,--no-as-needed,--allow-shlib-undefined,--start-group $(CLIENT_LIBS) -lgcc -Wl,--end-group \
	  $(shell $(ARMCC) -print-file-name=crtend.o) $(shell $(ARMCC) -print-file-name=crtn.o) -o $@

database: $(ARTIFACTS)/db-engine-probe $(ARTIFACTS)/zte-meshctl $(ARTIFACTS)/ns-run $(ARTIFACTS)/config-bridge-probe

$(ARTIFACTS)/db-engine-probe: db-engine-probe.c controller-run.c controller-service.c controller-service.h controller-packet.c controller-wsc.c controller-wsc-crypto.c controller-wsc-crypto.h controller-packet.h startup-views.inc multiap-cfg.c multiap-cfg.h multiap_cfg.def \
                           agent-sync.c agent-sync.h wlan_cfg.def wlan_psk.def \
                           mesh-policies.c mesh-policies.h multiap_master.def multiap_domain.def vendor-db.h \
                           wlan-profiles.c wlan-profiles.h wlan_base.def wlan_cfg.def wlan_ssid.def \
                           wlan_psk.def dev_info.def mesh-bridge.c mesh-bridge.h Makefile $(ARTIFACTS)/manifest.json
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -no-pie -nostdlib \
	  $(shell $(ARMCC) -print-file-name=crt1.o) $(shell $(ARMCC) -print-file-name=crti.o) \
	  $(shell $(ARMCC) -print-file-name=crtbegin.o) db-engine-probe.c controller-run.c controller-service.c controller-packet.c controller-wsc.c controller-wsc-crypto.c agent-sync.c multiap-cfg.c mesh-policies.c wlan-profiles.c mesh-bridge.c \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
	  -Wl,--no-as-needed,--start-group $(VENDOR_LIBS) -lgcc -Wl,--end-group \
	  $(shell $(ARMCC) -print-file-name=crtend.o) $(shell $(ARMCC) -print-file-name=crtn.o) -o $@

$(ARTIFACTS)/ns-run: ns-run.c controller-net.c
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror $(NSRUN_FLAGS) ns-run.c controller-net.c -o $@

# Static ARM32 client for the factory daemons' sendcmd debug channel (a lab tool, not part of the runtime).
$(ARTIFACTS)/zte-debugcmd: zte-debugcmd.c
	$(ARMCC) -O2 -std=gnu11 -Wall -Wextra -Werror -march=armv7-a -mfloat-abi=soft -static zte-debugcmd.c -o $@

# The same launcher for tmpfs lab runs (dev-service-smoke.py and friends unpack the runtime to /tmp/zte-mesh-svc).
$(ARTIFACTS)/ns-run-svctest: ns-run.c controller-net.c
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror -DSERVICE_ROOT='"/tmp/zte-mesh-svc/root"' ns-run.c controller-net.c -o $@

$(ARTIFACTS)/config-bridge-probe: config-bridge-probe.c mesh-bridge.c mesh-bridge.h
	$(ROUTERCC) -O2 -std=c11 -static -Wall -Wextra -Werror config-bridge-probe.c mesh-bridge.c -o $@

$(LIBDIR)/libzte_mesh_shim.so: shim.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_mesh_shim.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(LIBDIR)/libzte_controller_profile.so: controller-profile.c cm-profile-layout.inc vendor-db.h mesh-bridge.h wlan_cfg.def wlan_psk.def
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_profile.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) controller-profile.c \
	  -Wl,--no-as-needed,--start-group $(VENDOR_LIBS) -lgcc -Wl,--end-group -o $@

$(LIBDIR)/libzte_controller_route.so: controller-route.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_route.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(LIBDIR)/libzte_controller_trace.so: controller-trace.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_controller_trace.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

# The child-side configuration-manager stand-in. Only the two WLAN setters are interposed, and each of them only
# reports success when the database row already holds the value the caller passed in (agent-mm.c).
$(LIBDIR)/libzte_agent_mm.so: agent-mm.c
	$(ARMCC) $(COMMON) -march=armv7-a -mfloat-abi=soft -shared -nostdlib \
	  -Wl,-z,defs -Wl,-soname,libzte_agent_mm.so \
	  -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) $< \
	  -l:libdl.so.2 -l:libc.so.6 -l:ld-linux.so.3 -lgcc -o $@

$(ARTIFACTS)/exec-check: exec-check.c
	$(ROUTERCC) -O2 -static -Wall -Wextra -Werror $< -o $@

$(ARTIFACTS)/qemu-arm-static:
	cp /usr/bin/qemu-arm-static $@
	chmod 700 $@

$(ARTIFACTS)/tests:
	mkdir -p $@

$(ARTIFACTS)/tests/libmockdb.so: tests/mockdb.c | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -Wall -Wextra -Werror -fPIC -shared $< -o $@

$(ARTIFACTS)/tests/libshim-host.so: shim.c | $(ARTIFACTS)/tests
	$(HOSTCC) $(COMMON) -DZTE_MESH_HOST_TEST -shared -Wl,-z,defs $< -ldl -o $@

$(ARTIFACTS)/tests/caller: tests/caller.c $(ARTIFACTS)/tests/libmockdb.so
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -L$(ARTIFACTS)/tests \
	  -Wl,-rpath,'$$ORIGIN' -lmockdb -o $@

$(ARTIFACTS)/tests/no-backend: tests/no-backend.c | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -ldl -o $@

$(ARTIFACTS)/tests/bridge-codec: tests/bridge-codec.c mesh-bridge.c mesh-bridge.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/bridge-codec.c mesh-bridge.c -o $@

$(ARTIFACTS)/tests/cm-profile-codec: tests/cm-profile-codec.c controller-profile.c cm-profile-layout.inc wlan_cfg.def wlan_psk.def vendor-db.h mesh-bridge.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/cm-profile-codec.c controller-profile.c -o $@

$(ARTIFACTS)/tests/controller-packet-codec: tests/controller-packet-codec.c controller-packet.c controller-packet.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror tests/controller-packet-codec.c controller-packet.c -o $@

$(ARTIFACTS)/tests/controller-wsc-codec: tests/controller-wsc-codec.c controller-wsc.c controller-packet.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	  tests/controller-wsc-codec.c controller-wsc.c -Wl,--gc-sections -o $@

# The 0x1018 codec needs the crypto unit, so it links OpenSSL rather than the structure-only parser above.
$(ARTIFACTS)/tests/controller-wsc-crypto-codec: tests/controller-wsc-crypto-codec.c controller-wsc-crypto.c controller-wsc-crypto.h controller-packet.h | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	  tests/controller-wsc-crypto-codec.c controller-wsc-crypto.c -Wl,--gc-sections $(HOSTCRYPTO) -o $@

# The agent synchroniser's pure part (plan text + change detection); the vendor symbols are stubbed in the test.
$(ARTIFACTS)/tests/agent-sync-codec: tests/agent-sync-codec.c agent-sync.c agent-sync.h wlan_cfg.def wlan_psk.def | $(ARTIFACTS)/tests
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -I. tests/agent-sync-codec.c agent-sync.c -o $@

test-host: $(ARTIFACTS)/tests/caller $(ARTIFACTS)/tests/no-backend $(ARTIFACTS)/tests/libshim-host.so $(ARTIFACTS)/ns-run $(ARTIFACTS)/tests/bridge-codec $(ARTIFACTS)/tests/cm-profile-codec $(ARTIFACTS)/tests/controller-packet-codec $(ARTIFACTS)/tests/controller-wsc-codec $(ARTIFACTS)/tests/controller-wsc-crypto-codec $(ARTIFACTS)/tests/agent-sync-codec
	$(ARTIFACTS)/tests/controller-wsc-codec
	$(ARTIFACTS)/tests/controller-wsc-crypto-codec
	$(ARTIFACTS)/tests/controller-packet-codec
	$(ARTIFACTS)/tests/agent-sync-codec
	$(ARTIFACTS)/tests/cm-profile-codec
	$(ARTIFACTS)/tests/bridge-codec
	python3 -m unittest discover -s tests -p test_evidence.py -v
	python3 -m unittest discover -s tests -p test_device_workspace.py -v
	python3 tests/test_schema.py $(abspath $(ARTIFACTS))
	python3 tests/test_policies.py $(abspath $(ARTIFACTS))
	python3 tests/test_profiles.py $(abspath $(ARTIFACTS))
	python3 tests/test_startup.py $(abspath $(ARTIFACTS))
	sh tests/test_init.sh
	sh tests/test_rpcd.sh
	sh tests/test_wlan_sync.sh
	python3 tests/test_i18n.py
	python3 tests/test_runtime.py
	@if command -v node >/dev/null 2>&1; then node tests/test_luci.js; else echo "test_luci: skipped (node is not installed)"; fi
	python3 tests/test_preload.py $(abspath $(ARTIFACTS)/tests)
	python3 tests/test_sandbox.py $(abspath $(ARTIFACTS)/ns-run)
