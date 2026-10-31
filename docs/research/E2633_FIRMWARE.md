# ZXHN E2633 官方固件解包记录（2026-10-29）

对象：`F:/中兴E2633转正/6B5改_109适配/6B5改_109适配.bin`，26 870 292 字节，
MD5 `6f004d68c2bf0551a8ab383a658e38b2`（与同目录 `md5.txt` 一致，文件完整）。

**先纠一个型号**：文件头写的是 `ZXHN E2633 V1.0.0`、版本 `V1.0.0.6B5.8000`、
构建日期 `20240925150510`。不是 E2631。两者同为 ZX279128S 平台，但不是同一款，
也不是本项目联调用的 SR1010（ZX279133）/ Z506。

## 1. 一句话结论

**内核完全解开了，rootfs 是 AES-128-ECB 加密的 JFFS2，密钥拿不到。** 能拿到的是
一份完整的内核 `.config` 和几条交叉验证过的事实；拿不到的是 `multiapd` / `libcmapi` / `cspd`
本体 —— mesh 的实现全在那里面。

> 2026-10-29 更新：结合 [Zlion-Y 的 E2631 移植记录](https://blog.zlion.top/posts/zte-e2631-mainline-linux-port/)
> 和其仓库 `overlay/package/firmware/e2631-fwslot/src/e2631-fwslot.c`，rootfs 的加密算法从上一版的
> 「笼统块加密」**收敛为 AES-128-ECB**，文件系统从「无 squashfs magic」**确定为 JFFS2**。

## 2. 容器布局

| 偏移 | 长度 | 内容 | 状态 |
|---|---|---|---|
| `0x000000` | 564 | ZTE 固件头：型号、版本、构建时间、分区偏移与长度、CRC | 明文 |
| `0x000234` | 3 342 742 | uImage（magic `27051956`，ARM，`load=0x40008000`，comp=0） | 明文，CRC `0x31f288df` 自洽 |
| `0x3303ca` | 65 098 | 零填充（对齐） | — |
| `0x340214` | 23 461 888 | rootfs | **AES-128-ECB 密文**，解密后是 JFFS2 |
| 尾部 | 512 | `0xFF` 明文块被 ECB 加密后的密文 `9b58f878…`，重复 5906 次 | — |

固件头字段（用 e2631-fwslot.c 的权威表解析）：

- `H_FS_FILEOFS @0x44` = `0x019a0000` = 26 869 760，比文件大小少 532 字节
- `H_SLOT0 @0x4c` = `0x00000234`，就是 uImage 的偏移（E2633 头与 E2631 的字段名有出入，但数值对得上）
- 版本串 `V1.0.0.6B5.8000` @ `0x24`；兼容串 `V1.0.9.25B4.6000` @ `0x1b2`；产品名 `ZXHN E2633 V1.0.0` @ `0x80`

E2633 与 E2631 的 inner_magic 不同（E2633 是 `33 33 33 33 cc cc cc cc 88 88 88 88 dd dd dd dd`，
E2631 是 `33 33 33 33 66 66 66 66 99 99 99 99 cc cc cc cc`），所以头部不能完全套用 E2631 的字段表。

rootfs 区长度是 16 的整数倍，起点却不 16 对齐（`0x340214 % 16 == 4`），
且全程熵 7.99、找不到 `hsqs`/`sqsh`/`shsq`/`qshs` 任何一种 squashfs magic。判为块加密。

## 3. 内核是怎么解开的

uImage 的 `comp=0` 只说明 uImage 这层没压缩，里面装的是 **zImage**，还得再解一层。
`0x294` 处是 zImage 头（`0x016f2818`），`0x214` 字节后是解压器，压缩流在 `0x681c`：

```
gzip @0x681c  ->  6 627 680 字节 ARM Image（开头 9e2900eb 00900fe1 = bl / mrs）
Linux version 4.1.25 (root@b9624da43cdf)
gcc version 4.9.3 (Buildroot 2015.08.1-svn1486) #2 SMP Wed Sep ...
```

解压后的内核里带 **IKCONFIG**（`IKCFG_ST` @ `0x42eae8`），直接拿到 92 250 字节的完整 `.config`。
这是本次最大收获 —— 不用猜哪些宏开了。

产物留在 `out/analysis/e2633/`（已被 `.gitignore` 的 `/out/` 覆盖）：

| 文件 | 内容 |
|---|---|
| `kernel-uimage.bin` | 3 342 742 B，uImage 原样 |
| `kernel-Image.bin` | 6 627 680 B，解压后的 ARM Image |
| `kernel.config` | 92 250 B，内嵌 `.config` |

复现命令（Python 3，无第三方库）：

```python
import zlib
d = open('6B5改_109适配.bin','rb').read()
img = zlib.decompressobj(16+zlib.MAX_WBITS).decompress(d[0x681c:], 8<<20)
i = img.find(b'IKCFG_ST'); j = img.find(b'\x1f\x8b\x08', i)
open('kernel.config','wb').write(
    zlib.decompressobj(16+zlib.MAX_WBITS).decompress(img[j:j+300000], 2<<20))
```

## 4. `.config` 里与我们相关的东西

### 4.1 无线栈：私有，没有 mac80211

```
CONFIG_CSP_WLAN=y
# CONFIG_CFG80211 is not set
# CFG80211 needs to be enabled for MAC80211
# CONFIG_WLAN is not set
CONFIG_WLAN_MTK_7916=y
```

和 [e2631-openwrt 的 v1.5 报告](https://github.com/Zlion-Y/zte-e2631-openwrt) 里从
`config.gz` 得到的结论**逐字一致**。两个机型、两条独立的取材路径，结论相同：
原厂无线不在标准栈里。这条要记下来，因为它把"从原厂提取参数套到 hostapd"彻底否掉了。

### 4.2 回程是驱动建的

```
CONFIG_CSP_WLAN_EXTENDER_LINK_BY_DRIVER=y
CONFIG_CSP_WLAN_MTK_EXTENDER=y
CONFIG_CSP_WLAN_CSPKERNEL_BRIDGE=y
# CONFIG_CSP_WLAN_ADAPTER_MULTI_AP is not set
```

`EXTENDER_LINK_BY_DRIVER` 直接回答了之前悬着的问题「回程 BSS 谁负责变成一条链路」：
**驱动**。而 `ADAPTER_MULTI_AP` 是关的 —— 驱动内部的 Multi-AP 适配层没启用，
但扩展器链路走的是驱动自己的路径（`MTK_EXTENDER`）。

对照内核符号，驱动侧确实有这套接口：

```
MULTIAP_SLAVE
wlan_get_wlan_multiap_mode
wlan_multiap_mode
```

这三条说明「代理/从属」在驱动里是有模式变量的，不是一个纯粹的用户态概念。
[AGENT_PROBE.md](AGENT_PROBE.md) §11.6 里「4 地址由 hostapd 依 `multi_ap` 去设」这个推断，
在原厂路径上不成立 —— 原厂根本不经过 hostapd。

### 4.3 内核里 1905 有自己的钩子

```
CONFIG_CSPKERNEL_IEEE1905=y
CONFIG_BRIDGE_NETFILTER=y
CONFIG_CSPKERNEL_BRIDGE=y

br_ieee1905_filter_init register nf hooks failed
br_ieee1905_check_is1905_msg
1905_msg
```

`br_ieee1905_filter_init` 注册的是 **netfilter 钩子**，挂在网桥上，用
`br_ieee1905_check_is1905_msg` 识别 1905 帧。也就是说原厂的 1905 数据面**穿过网桥时会被内核专门识别**，
不是一束普通的以太网广播。

这一条解释了实验室与真机的一个潜在差异：lab 里 1905 走 `veth` + `br-lan`，
内核把它当普通帧泛洪；真机上有这个钩子参与。它可能是为了做
「1905 只往 MAP 口送、不往用户口送」或者「按 1905 目的 MAC 定向复制」。
**没拿到内核源码，具体行为是推断。**

### 4.4 能力位（对我们已知功能的交叉印证）

| 配置 | 值 | 对应我们已经实现的 |
|---|---|---|
| `CSP_WLAN_SUPPORT_80211KV` | y | 漫游只到 k/v，**没有 r**。与 [Z506 无 FT 的结论](../CONTROLLER_CAPABILITIES.md) 一致 |
| `CSP_WLAN_DUAL_BAND_SETTING_SYNC` | y | `wifi_dual_band_sync` 是真机原生行为，不是我们发明的 |
| `CSP_WLAN_KEY_GLOBAL_SETTING` | y | 全局密钥，对应 M2 里一份凭据下发给多个 BSS |
| `CSP_WLAN_VAP_CONFIG_INDEPENDENT` | y | 每个 BSS 独立配置，对应 `WLANCfg` 的多行槽位 |
| `CSP_WLAN_11AX` / `WPA3` / `160MHZ` | y | E2633 是 AX3000 级，比 Z506 新 |
| `CSP_WLAN_VLAN` / `ELINK` | y | 我们有 VLAN 与 elink 无通道，符合原厂有而我们没有 |
| `CONFIG_SMP` / `PREEMPT_NONE` / `HZ=100` | y/100 | `mem=128M console=ttyAMA0,115200n8` |

### 4.5 mesh 专项宏：全部关闭

```
# CONFIG_CSPKERNEL_WPS_SEND_MESH is not set
# CONFIG_CSPKERNEL_WANLAN_MESH is not set
# CONFIG_CSPKERNEL_WBDEV_IEEE1905 is not set
# CONFIG_CSPKERNEL_WBDEV_FLOOD_IEEE1905 is not set
```

只有 `CONFIG_CSPKERNEL_IEEE1905=y` 开着。这与 e2631-openwrt 观察到的
`MeshId=`（空）、`MeshWPAKEY=`（空）、`MapMode=0` 是同一件事的两个视角：
MTK 自带的 mesh 关着，真正干活的是 ZTE 自己的 1905 实现。

## 5. rootfs 为什么没解开

**算法已确定，密钥拿不到。**

### 5.1 算法：AES-128-ECB（双重确认）

参考记录 [`e2631-fwslot.c`](https://github.com/Zlion-Y/zte-e2631-openwrt) 头部注释原话：

> only the stock rootfs is AES-128-ECB at rest, and that decryption lives in the
> vendor kernel's MTD read path, so a mainline kernel reads plain bytes.

与实测指纹完全吻合——**ECB 的致命特征**：JFFS2 未使用的区域是 `0xFF` 填充，
ECB 模式下「相同明文块 → 相同密文块」，于是密文里 `9b58f878bbb341366b2b543daffb0185`
这一个 16 字节块重复了 **5906 次**（`AES_ECB(0xFF…FF)` 的结果）。CBC/CTR 不会有这种重复。

所以上一版说「笼统的块加密」不够精确；也不是 squashfs（squashfs magic 四种变体全无），
解密后是 **JFFS2**（`CONFIG_JFFS2_FS=y`，与参考记录的 rootfs/kmodule 分区描述一致）。

### 5.2 密钥：由厂商运行时推导，不公开

参考记录明说：**「密钥由厂商内置逻辑运行时推导，具体密钥与推导方法不在本文提供」**，
即 `zte_token` 派生。整个 e2631-openwrt 仓库和四个博客文章里都没有解密工具、没有密钥。
内核 `.config` 里 `CONFIG_CRYPTO_AES=y` 但 `# CONFIG_CRYPTO_CBC is not set`、
`# CONFIG_CRYPTO_XTS is not set`——AES 是给「MTD 读路径的透明解密」用的，
解密在内核态发生，密钥不落盘，**不可能从固件包本身反推**（128 位 AES 无法穷举）。

### 5.3 结论

以现有材料（这个 `.bin` + 公开参考资料）**解不开 rootfs**。要拿到 `multiapd` / `libcmapi`，
只有三条路，且都有明确代价：

| 路 | 怎么做 | 代价 / 对本项目的增量 |
|---|---|---|
| 读 bootloader 取密钥 | TTL 串口（115200）进 U-Boot，密码 `5cE080@fyBD`，逆向密钥推导 | 需要拆机 + 逆向 U-Boot 的 token 派生逻辑，工作量未评估 |
| 真机 root 直接拷 | telnet 开后 `cat /usr/sbin/multiapd` 等 | **无增量**：SR1010 那份我们已有，mesh 实现相同 |
| 第三方解包工具 | 社区对 ZTE 家用机有若干（见 §7） | 版本相关，不保证命中 E2633/6B5 |

**关键判断**：第三条路其实指向「从真机读」，而不是「解这个 bin」——因为密钥在
U-Boot 运行时才推导出来，任何纯软件解包工具都得先拿到密钥，而密钥又不在包内。
换句话说，**对「复现 mesh」这个目标，解密这个 bin 的边际价值接近零**：SR1010 上
已经拿到同一套 `multiapd`/`libcmapi`，解 E2633 的 rootfs 不会多出任何新代码。

## 6. 对「复现中兴 mesh」到底有多少帮助

分四种用途看，别混在一起说：

| 想做的事 | 这个固件能不能帮上 | 理由 |
|---|---|---|
| 提取 mesh 实现、移植代码 | **不能** | rootfs 加密，`multiapd`/`libcmapi`/`cspd` 都在里面 |
| 弄清原厂架构（用不用标准栈、谁建回程） | **能，而且是硬证据** | 完整 `.config` 拿到了 |
| 验证我们已逆向的协议常量 | **不能** | `MAP24G`/`MAP5GL`/`0x1018` 这些在用户态库里，内核里没有 |
| 判断 E2633 能不能刷我们的方案 | **能，而且是否定的** | 见下 |

最后一行需要展开：E2633 的 Wi-Fi 是 `CSP_WLAN` + `mt7916` 私有栈，
**没有 cfg80211/mac80211**。而我们的 `wlan-sync` 落的是 UCI + `wifi reload`，
即 hostapd + mac80211 那条路。在 E2633 的原厂固件上，这套没有落点 ——
这不是适配问题，是要换整个无线栈（这正是 e2631-openwrt 项目在做的事：
把原厂 `mt7916.ko` 换成 mt76 + mac80211）。

所以这个固件的实际价值是**三张证据**，不是一堆代码：

1. **第二个机型独立确认了「原厂无线不在标准栈里」** —— 之前只有 e2631-openwrt 一个来源
2. **`EXTENDER_LINK_BY_DRIVER=y` 明确了回程归驱动** —— 我们之前对 Z506 的推断得到验证
3. **`br_ieee1905_filter_init` 说明 1905 在内核网桥有专门钩子** —— 这是之前完全不知道的，能解释 lab 与真机可能的行为差异

它在什么时候才真的有用：如果你想**给 E2633 做 OpenWrt 移植**，
这份 `.config` 是现成的对照表（哪些原厂功能要在新栈上重新实现）。

## 7. 参考资料与可复用的设备侧事实

五个来源（Zlion-Y 博客 + 两个 GitHub 仓库）：

1. [E2633 刷入官方 AX3000（TTL 篇）](https://blog.zlion.top/posts/e2633-research-ttl/)
2. [E2633 转正官方版分析（Telnet 篇）](https://blog.zlion.top/posts/e2631-research-telnet/)
3. [Z500/Z503/Z507 升级小方糖公版系统](https://blog.zlion.top/posts/z503-to-public/)
4. [中兴巡天 AX3000 (E2631) 主线 Linux 内核移植全记录](https://blog.zlion.top/posts/zte-e2631-mainline-linux-port/)
5. [Zlion-Y/e2631-mainline-zx279128s](https://github.com/Zlion-Y/e2631-mainline-zx279128s) / [Zlion-Y/zte-e2631-openwrt](https://github.com/Zlion-Y/zte-e2631-openwrt)

从这些资料里提炼的、对本项目有用的设备侧事实（**与本仓的 SR1010/Z506 无直接关系，但同一 ZTE mesh 血统**）：

| 项 | 值 | 备注 |
|---|---|---|
| SoC | ZX279128S，双核 Cortex-A9 @1GHz，**无 VFP/NEON/idiv** | 与 SR1010 同 A9 软浮点 |
| U-Boot 进入 | 上电按 `1`，密码 `5cE080@fyBD` | 串口 115200-8-N-1，`earlycon=zteuart`，基址 `0x94404000` |
| 分区表 | bootloader / kernel(AES-ECB) / rootfs(JFFS2) / kmodule(JFFS2) / tag / wifi / config | telnet `cat /proc/mtd` 可见 |
| telnet 开启 | Web 导出 `config.bin` → 一键脚本改 → 还原 | 默认账密 admin |
| 双槽 | slot0 @ `0x700000`、slot1 @ `0x2300000`，各 28 MiB | cspboot 按「版本头 CRC1 + serial 高者」选槽 |
| 刷写 | `fw_flashing -f <pkg>` 只写备用槽 | Web 上传被 HTTP 请求体限制，大包必须走 TFTP |
| 内核 | Linux 4.1.25，gcc 4.9.3，Buildroot 2015.08.1，`mem=128M` | 内嵌 IKCONFIG，本文 §3/§4 已解出 |

**这条「TTL 进 U-Boot + telnet 开 root」的链路本身是可复用的**：如果哪天要验证 E2633 上的
原厂 mesh 行为（而不是 SR1010），可以照这条路进设备。但注意它与我们已联调的 Z506 不是同一款，
且 E2633 原厂 mesh（`MeshId=`/`MapMode=0`）是关着的，参考价值有限。

## 8. 归档位置

- 本文件：`docs/research/E2633_FIRMWARE.md`
- 产物：`out/analysis/e2633/{kernel-uimage.bin, kernel-Image.bin, kernel.config}`（不入库）
- 原固件：`F:/中兴E2633转正/6B5改_109适配/`（仓库外）
