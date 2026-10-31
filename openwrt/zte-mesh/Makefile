# SPDX-License-Identifier: GPL-2.0-only
#
# The ZTE multiapd Controller runtime for the SR1010. The runtime tarball with
# the vendor binaries is committed next to this Makefile; regenerate it with
# `sh tools/build/runtime.sh install`. ZTE_MESH_RUNTIME can point at another one.
#
# Everything this package installs is data: shell scripts, the rpcd plugin, the
# UCI defaults and the runtime tarball. That is why PKGARCH is "all" -- an x86_64
# lab box can install it, which it could not while this package also carried the
# compiled launcher. The two binaries (ns-run, mesh-search) are machine code and
# have their own arch-tagged package; see ../zte-mesh-launcher/Makefile for why
# they cannot simply live here.
include $(TOPDIR)/rules.mk

PKG_NAME:=zte-mesh
PKG_VERSION:=0.1.0
PKG_RELEASE:=10
PKG_LICENSE:=GPL-2.0-only
# The tarball carries proprietary ARM32 vendor code; keep it out of shared builds.
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/zte-mesh
  SECTION:=net
  CATEGORY:=Network
  TITLE:=ZTE mesh controller runtime (sandboxed vendor multiapd)
  # No target gate and no arch: this half is scripts and data. The parts that do
  # care about the board -- the compiled launcher, the kernel modules, iproute2 --
  # are declared by zte-mesh-launcher, which is arch-tagged and pulls this package
  # in. A board that installs this alone has no ns-run and the init script says so.
  DEPENDS:=
  PKGARCH:=all
endef

define Package/zte-mesh/description
  Runs the ZTE multiapd Controller in a private IPC/network/mount namespace
  and attaches its wire to the LAN bridge. This package is architecture
  independent; zte-mesh-launcher supplies the compiled ns-run and mesh-search
  for the board. Running the vendor daemon additionally needs AArch32 compat
  (CONFIG_COMPAT) and SysV IPC in the kernel.
endef

define Package/zte-mesh/conffiles
/etc/config/zte_mesh
endef

ZTE_MESH_RUNTIME ?= $(CURDIR)/zte-mesh-runtime.tar.gz

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	tar -C $(PKG_BUILD_DIR) -xzf $(ZTE_MESH_RUNTIME)
endef

# Nothing is compiled here. ns-run and mesh-search used to be built in this package, but they
# are machine code and cannot ride in an arch:all apk: on the x86_64 lab box an aarch64 ns-run
# would simply not exec. They moved to ../zte-mesh-launcher, which is arch-tagged and depends
# on this one. The sources those two are built from still travel inside the runtime tarball --
# that is what stops them drifting (FEATURE_STATUS C5) -- and the launcher unpacks them from
# there, so there is exactly one copy of each.
#
# Build/Compile has to be overridden rather than merely left out. Undefined, it falls back to
# Build/Compile/Default, which runs $(MAKE) in the build directory -- and that directory is just
# the unpacked tarball, so the build dies with "No targets specified and no makefile found".
# (Build/Configure/Default is safe to leave alone: it is guarded by [ -x ./configure ].)
define Build/Compile
	true
endef

define Package/zte-mesh/install
	$(INSTALL_DIR) $(1)/usr/lib/zte-mesh $(1)/usr/libexec/zte-mesh
	$(CP) $(PKG_BUILD_DIR)/root $(1)/usr/lib/zte-mesh/
	$(INSTALL_BIN) ./files/attach $(1)/usr/libexec/zte-mesh/attach
	$(INSTALL_BIN) ./files/wlan-sync $(1)/usr/libexec/zte-mesh/wlan-sync
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/zte-mesh.init $(1)/etc/init.d/zte-mesh
	$(INSTALL_CONF) ./files/zte-mesh.config $(1)/etc/config/zte_mesh
	$(INSTALL_BIN) ./files/luci.zte-mesh $(1)/usr/libexec/rpcd/luci.zte-mesh
endef

$(eval $(call BuildPackage,zte-mesh))
