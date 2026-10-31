# SPDX-License-Identifier: GPL-2.0-only
#
# The two compiled halves of the ZTE mesh controller:
#
#   ns-run      builds the private IPC/network/mount namespace, execs the vendor
#               daemon inside it and wires the sandbox to the LAN bridge
#   mesh-search the controller half of the enrolment handshake, the stand-in for
#               the cspd search trigger the sandbox does not have (FEATURE_STATUS
#               B18); started by the "search" instance in zte-mesh.init
#
# They have their own package for exactly one reason: they are machine code.
# zte-mesh itself is PKGARCH:=all -- init scripts, the rpcd plugin and the
# runtime tarball are all data, so an x86_64 lab box can install it -- but a
# binary produced by $(TARGET_CC) cannot ride along in an arch:all apk, it would
# not exec there. package-pack.mk reads one global PKGARCH per Makefile
# ("Architecture: $(PKGARCH)"), so there is no per-package override: two
# architectures need two package directories.
#
# The sources are deliberately not duplicated here. They travel inside the
# runtime tarball next to ../zte-mesh/Makefile -- that is what keeps them from
# drifting away from the repository (FEATURE_STATUS C5: a stale ns-run in a
# tarball once cost a whole day) -- and are unpacked from it at build time.
include $(TOPDIR)/rules.mk

PKG_NAME:=zte-mesh-launcher
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/zte-mesh-launcher
  SECTION:=net
  CATEGORY:=Network
  TITLE:=ZTE mesh controller launcher and search helper
  # The board has to be able to run what the sandbox holds: aarch64_cortex-a53
  # with AArch32 compat (CONFIG_COMPAT) and SysV IPC in the kernel. Two targets
  # are known to carry that: the ZTE ZX279133 this was written for, and generic
  # IPQ807x boards. The dependency exists so a wrong board fails at install time
  # instead of at exec time.
  #
  # +kmod-veth is a hard requirement, not a nicety: the private wire is a veth
  # pair and without it the launcher stops with errno=2.
  DEPENDS:=@(TARGET_zte||TARGET_qualcommax) +zte-mesh +ip-full +kmod-veth +kmod-sched-core
  PKGARCH:=aarch64_cortex-a53
endef

define Package/zte-mesh-launcher/description
  Supplies /usr/libexec/zte-mesh/ns-run and mesh-search for the board, built
  from the launcher sources that travel inside the zte-mesh runtime tarball.
  Architecture specific by nature; zte-mesh itself is arch:all and installs
  everything else.
endef

# The tarball lives in the sibling package directory, which is the one place it
# is committed. Point ZTE_MESH_RUNTIME elsewhere to build against another one.
ZTE_MESH_RUNTIME ?= $(CURDIR)/../zte-mesh/zte-mesh-runtime.tar.gz

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	tar -C $(PKG_BUILD_DIR) -xzf $(ZTE_MESH_RUNTIME) ns-run.c controller-net.c enroller.c
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -O2 -Wall -o $(PKG_BUILD_DIR)/ns-run \
		$(PKG_BUILD_DIR)/ns-run.c $(PKG_BUILD_DIR)/controller-net.c
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -O2 -Wall -o $(PKG_BUILD_DIR)/mesh-search \
		$(PKG_BUILD_DIR)/enroller.c
endef

define Package/zte-mesh-launcher/install
	$(INSTALL_DIR) $(1)/usr/libexec/zte-mesh
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ns-run $(1)/usr/libexec/zte-mesh/ns-run
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mesh-search $(1)/usr/libexec/zte-mesh/mesh-search
endef

$(eval $(call BuildPackage,zte-mesh-launcher))
