# SPDX-License-Identifier: GPL-2.0-only
#
# The ZTE multiapd Controller for OpenWrt: the vendor runtime tarball with the ARM32 binaries, the
# scripts and UCI defaults that drive it, and the launcher that puts the whole thing in a private
# namespace. The tarball is committed next to this Makefile; regenerate it with
# `sh tools/build/runtime.sh install`. ZTE_MESH_RUNTIME can point at another one.
#
# Everything the controller needs on a board is in this one package. The launcher used to be a second
# one, on the reasoning that an ELF names one architecture while everything else here is data and
# package-pack.mk reads one PKGARCH per Makefile. That reasoning ran out when the launcher became
# arch:all as well: it carries one build per CPU (aarch64, armv7, x86_64) under
# /usr/libexec/zte-mesh/arch/ and a two-name shell entry that picks the directory matching `uname -m`
# (files/launcher-dispatch). Both halves are arch:all now, so the split stopped buying anything and
# started costing a moment: two packages are two transactions, and an upgrade that lands one before
# the other leaves the box running a controller from one package against a launcher from the other
# (measured on the lab router on 2026-11-05: /tmp/log/apk.log is the record). One apk, one
# `apk add`, one moment at which the two halves are the same version.
#
# What arch:all gives up is the install-time rejection. `DEPENDS:=@(TARGET_zte||TARGET_qualcommax)`
# used to make `apk add` fail on a board that could not run the sandbox; an arch:all package installs
# by definition, so the question moved to run time: the dispatcher names the machine it is standing
# on, and zte-mesh.init asks the same thing before it starts an instance (procd would otherwise respawn
# a 127 every five seconds). The same holds for the CPU list -- a machine the package does not carry
# installs it and is told so, rather than being refused by the solver.
#
# The launcher sources are not duplicated here either. ns-run.c, controller-net.c and enroller.c travel
# inside the runtime tarball -- that is what keeps them from drifting away from the repository
# (FEATURE_STATUS C5: a stale ns-run in a tarball once cost a whole day) -- and Build/Prepare unpacks
# the tarball once, so the code this package compiles is the code it ships.
include $(TOPDIR)/rules.mk

PKG_NAME:=zte-mesh
PKG_VERSION:=0.1.0
PKG_RELEASE:=23
PKG_LICENSE:=GPL-2.0-only
# The tarball carries proprietary ARM32 vendor code; keep it out of shared builds.
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/zte-mesh
  SECTION:=net
  CATEGORY:=Network
  TITLE:=ZTE mesh controller runtime (sandboxed vendor multiapd, all architectures)
  # No target gate and no named architecture: the scripts and data do not care about the board, and
  # the compiled launcher inside this package carries one build per CPU and refuses a machine it does
  # not have at run time. What does have to be true is that the sandbox can be built at all: the
  # private wire is a veth pair, so without kmod-veth the launcher stops with
  # "error=private-service-network errno=2", and attaching it to the bridge needs iproute2.
  DEPENDS:=+ip-full +kmod-veth +kmod-sched-core
  PKGARCH:=all
endef

define Package/zte-mesh/description
  Runs the ZTE multiapd Controller in a private IPC/network/mount namespace and attaches its wire to
  the LAN bridge. One build of the launcher per supported CPU (aarch64, armv7, x86_64) plus the shell
  entry that picks the one this machine runs. Running the vendor daemon additionally needs AArch32
  compat (CONFIG_COMPAT) and SysV IPC in the kernel.
endef

define Package/zte-mesh/conffiles
/etc/config/zte_mesh
endef

ZTE_MESH_RUNTIME ?= $(CURDIR)/zte-mesh-runtime.tar.gz
# The multi-architecture build script sits at the top of the repository, next to the other build
# helpers, so the compiler search has one home. The tarball above is inside the feed and this is not:
# a checkout that exports only the package directories (a feed with no tools/ in it) will not find it,
# and Build/Compile says so instead of letting `sh` fail on an empty argument. Set
# ZTE_MESH_LAUNCHER_BUILD to the script in that case.
ZTE_MESH_LAUNCHER_BUILD ?= $(CURDIR)/../../tools/build/launcher-arches.sh

# One unpack, two uses: Build/Compile takes ns-run.c / controller-net.c / enroller.c out of the same
# tree that Build/Install prepares as the package payload, so the packaged sources and the packaged
# runtime cannot be two different versions of each other.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	tar -C $(PKG_BUILD_DIR) -xzf $(ZTE_MESH_RUNTIME)
endef

# The launcher is the only machine code here, and it is built for every architecture the package
# carries rather than for the one this tree targets. $(TARGET_CC) is the only toolchain an OpenWrt
# build tree guarantees, so it covers this tree's own CPU; the other CPUs come from host cross
# compilers where they are installed. Which CPUs got built is printed on purpose: an apk that carries
# one architecture is a package that installs everywhere and runs in one place, and the build log is
# where the difference is visible.
define Build/Compile
	[ -r $(ZTE_MESH_LAUNCHER_BUILD) ] || { \
		echo 'zte-mesh: no build script at $(ZTE_MESH_LAUNCHER_BUILD); set ZTE_MESH_LAUNCHER_BUILD to tools/build/launcher-arches.sh' >&2; \
		exit 1; \
	}
	ZTE_MESH_TARGET_CC="$(TARGET_CC)" \
	ZTE_MESH_TARGET_CFLAGS="$(TARGET_CFLAGS)" \
	sh $(ZTE_MESH_LAUNCHER_BUILD) $(PKG_BUILD_DIR) $(PKG_BUILD_DIR)/arch
endef

define Package/zte-mesh/install
	$(INSTALL_DIR) $(1)/usr/lib/zte-mesh $(1)/usr/libexec/zte-mesh
	$(CP) $(PKG_BUILD_DIR)/root $(1)/usr/lib/zte-mesh/
	# One file installed under two names -- the entry script is the program the name it was called by,
	# and it execs the build under arch/<cpu>/ that matches this machine.
	$(INSTALL_BIN) ./files/launcher-dispatch $(1)/usr/libexec/zte-mesh/ns-run
	$(CP) $(1)/usr/libexec/zte-mesh/ns-run $(1)/usr/libexec/zte-mesh/mesh-search
	$(CP) $(PKG_BUILD_DIR)/arch $(1)/usr/libexec/zte-mesh/
	$(INSTALL_BIN) ./files/attach $(1)/usr/libexec/zte-mesh/attach
	$(INSTALL_BIN) ./files/wlan-sync $(1)/usr/libexec/zte-mesh/wlan-sync
	$(INSTALL_BIN) ./files/sta-collect $(1)/usr/libexec/zte-mesh/sta-collect
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/zte-mesh.init $(1)/etc/init.d/zte-mesh
	$(INSTALL_CONF) ./files/zte-mesh.config $(1)/etc/config/zte_mesh
	$(INSTALL_BIN) ./files/luci.zte-mesh $(1)/usr/libexec/rpcd/luci.zte-mesh
endef

$(eval $(call BuildPackage,zte-mesh))
