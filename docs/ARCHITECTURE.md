# 架构：原厂 multiapd 在 OpenWrt 上当主控

## 1. 原理

原厂 mesh 的核心是用户态 `multiapd`：IEEE 1905、Multi-AP（EasyMesh）控制逻辑加中兴私有扩展。它不是 802.11s 或 batman-adv，
OpenWrt 的 `mesh_id`、usteer、DAWN 都换不来与原厂子节点的互通。

本项目不重写协议，而是把原厂 ZXSLC SR1010 固件里同一版本的 `multiapd` 和它的 27 个库搬到 OpenWrt 上，在私有命名空间里当控制器运行，
用"垫片"提供它依赖的环境，再用一个配置文件间接指挥它。线上的报文几乎全部由原厂守护进程自己构造；服务自己构造的只有信道选择
请求（CMDU `0x8006`）。实测与原厂主控对比（[REPLAY_RESULTS](research/REPLAY_RESULTS.md)）：M2、`PolicyConfigReq`、Renew 和
额外网络那几条消息除 MID 外逐字节相同；已知差别是 `MetricRptPolicy` 里无线电的顺序（不固定）、`CombinedInfraMetrics` 私有 TLV 的内容
（取决于邻居状态）、`0200fe` 消息（原厂入网时 3 条，我们上线约 15 秒后 1 条）。

## 2. 运行时结构

```text
UCI /etc/config/zte_mesh ── LuCI 页面（luci-app-zte-mesh）
        │                       │ ubus
        ▼                       ▼
/etc/init.d/zte-mesh        rpcd 插件 luci.zte-mesh（status / restart / reboot_node / light_node）
  write_config：UCI → /run/zte-mesh/config（0600，key=value，服务再校验一遍）
  procd 实例 controller：ns-run                    procd 实例 attach：把 zmesh0 挂进 br-lan
        │                                                       ▲
        ▼                                                       │ veth
ns-run（本机 CPU 的那一份静态构建；外层是入口脚本，见第 5 节）：新建 network/IPC/mount 命名空间，私有根 /usr/lib/zte-mesh/root，只把 /run/zte-mesh 暴露给宿主
        │
        ▼
db-engine-probe --controller-service（ARM32，原厂 glibc 2.26）
  ├─ 原厂共享内存数据库的服务端（libdb），表结构从 cspd 还原，启动时写入配置行
  ├─ 监督守护进程：启动、按需 SIGKILL 重启（mesh 级变化）、调度器、节点动作、写 state.json
  └─ fork/exec zte-meshctl（liboss_pub 客户端）发原厂 CM 通知 / ASEND
        │
        ▼
原厂 multiapd + preload 库
  libzte_mesh_shim.so            数据库初始化边界（DBShmCliInit / dbAPIGetView）
  libzte_controller_profile.so   CM 替身：CmWLANGetAPConfig、CmWLANGetPsk 从我们的行读
  libzte_controller_route.so     接口身份适配；cspd 的 multiapd_mgr 替身（启动计数 0xa96e）
  libzte_controller_trace.so     受限日志；子节点帧摘要写 /run/zte-mesh/peers/；子节点这边替 `0x800b` 答终端数
        │ IEEE 1905（EtherType 0x893a）
        ▼
zmesh0 ─ br-lan ─ 子路由的 multiapd ─ 子路由 cspd ─ 无线驱动
```

- **不运行 `cspd`**：守护进程需要的"路由器其余部分"由数据库行、CM 替身、身份环境变量和几个替身消息提供。
- **配置**：宿主只写 `/run/zte-mesh/config`；服务每秒检查，改动按种类热更新（写库后发原厂 CM 通知、按频段 Renew），
  mesh 级变化重启守护进程（原厂 `multiapd_mgr` 的做法），少数键的出现或消失要重启服务（见 [SERVICE.md](SERVICE.md)）。
- **状态**：服务每秒写 `/run/zte-mesh/state.json`（节点、动作结果、调度状态），rpcd 和 LuCI 读它。
- **子节点的终端数从哪来（实例 `sta-collect`）**：原厂 agent 的 `0x94` 是驱动经 `cspd` 填的，沙箱两样都没有，`ns-run` 又不挂
  `/sys`——实测它收到 AP Metric Query（`0x800b`）一声不响，所以 OpenWrt 子节点在页面上只能是"没数据"。数只能在宿主测：
  `sta-collect` 每 5 秒把本机 AP 的关联数写进 `/run/zte-mesh/ap-sta`（`epoch=` 加每路一行 `BSSID 数`，整表原子替换），
  这个目录本来就 bind 进私有根，`agent-ap-metrics.c` 在沙箱里读它拼 `0x800c`。表超过 30 秒没刷新就不回——
  **过期即沉默，不报一个陈旧的数**。主控那头由服务定时发查询（见 [SERVICE.md](SERVICE.md)）。

- **有线口的自愈（实例 `attach`）**：沙箱这头的 `veth-lab` 和宿主那头的 `zmesh0` 是一对 veth，删掉任一端两个一起没了；
  而沙箱里没有 `/proc`、没有 `/sys`、没有 `ip`、也没有 `CAP_NET_ADMIN`，daemon 又在 `bind()` 时把 ifindex 绑死了——
  线掉了里面没人能重建，只有重启实例才行。`attach` 因此兼任看门狗：`zmesh0` 与 `lab-peer` 连续 20 秒都不在、
  而沙箱进程还活着，就给它发 `SIGTERM`，procd 重新拉起（`respawn 3600 5 0`，retry 0 = 不限），新线由同一个循环改名入桥。
  这等于把"重启主控就好了"变成自动的。同样因为 retry：`search` 每 300 秒按设计退出一次，写成裸 `respawn` 会被
  procd 默认的 5 次预算在半小时内烧光然后永久不再启动（实测 22:40 启动、23:10 死掉），之后开机的子路由再也找不到主控。

## 3. 源文件

下表里的 `.c`/`.h`/`.def`/`.inc` 都在 `src/`；取证与构建脚本（`prepare.py`、`schema.py`、`cm-profile-evidence.py` 等）
在 `tools/dev/`；装机的 shell 与页面在 `openwrt/zte-mesh/files/`，随包分发的 `ns-run.c`/`controller-net.c`/`enroller.c`
用的是 tar 成员名，和 `src/` 这个目录名无关。

| 文件 | 运行位置 | 职责 |
|---|---|---|
| `ns-run.c`、`controller-net.c` | 路由器，本机 CPU 的那一份静态构建（aarch64 / armv7 / x86_64 各一） | 沙箱启动器：命名空间、私有根、私有网桥和到宿主的 veth |
| `db-engine-probe.c`、`controller-run.c`、`controller-service.c` | 沙箱，ARM32 | 数据库服务端、守护进程的启动与重启、长期运行的服务（配置、调度器、动作、状态） |
| `multiap-cfg.c`、`mesh-policies.c`、`wlan-profiles.c`、`*.def`、`startup-views.inc` | 同上 | 从原厂二进制还原的表结构与写表逻辑（带读回） |
| `mesh-bridge.c` | 同上 | 配置值的类型化校验（`mb_profile`、`mb_validate`） |
| `zte-meshctl.c` | 沙箱，ARM32 | 原厂 `diagget` 式的 OSS 客户端：CM 通知、Renew、重启/指示灯等 ASEND |
| `shim.c` | 沙箱，preload | 数据库初始化边界 |
| `controller-profile.c`、`cm-profile-layout.inc` | 沙箱，preload | CM 替身；43 个字段偏移由 `cm-profile-evidence.py` 从机器码推出 |
| `controller-route.c` | 沙箱，preload | 接口身份；`SSEND(0xa96e)` 启动计数替身 |
| `controller-trace.c` | 沙箱，preload | 受限日志、ASEND 记录、把收到的 1905 帧交给 `controller-peer.c`，并把 `0x800b` 交给 `agent-ap-metrics.c` 应答（回帧从查询到达的那个 socket 直接发出，不走本文件的 `sendto` 包装）；每一行先过 `controller-log.c` 的预算 |
| `controller-log.c`、`controller-log.h` | 沙箱，preload + 主机测试 | 纯函数：一段格式文本 + 调用方给的时钟 → 这一行打不打、打不成时发哪条提示（措辞固定，只有采样那句带一个累计丢弃数——值行打的是 daemon 格式化好的结果，提示里绝不掺它）。设备的日志落点是 logd 那圈 128 KiB 环，一次配置推送里原厂 daemon 重复的状态行能把它占满，所以按类分预算、重复行按分钟采样；带密钥/SSID/BSS 的格式永不展开值 |
| `controller-peer.c`、`controller-peer.h` | 沙箱，preload | 一帧 → `peers/<MAC>.<kind>`：指示灯、终端数、BSS 数、直连还是中继、动作回执；终端数还**按 BSSID 各存一个 `peers/<MAC>.<BSSID>.sta`**（各自的时间戳，服务读 `controller_peer_bss_load()` 把没过期的一路一路加起来，并拿到按 BSSID 的明细去拼 `state.json` 的 `bss_detail`——摘要只留最后一帧的总数，按射频分别上报时会互相顶掉）；落盘目录是 `ZTE_MESH_PEERS_DIR`，所以主机测试能在临时目录上验整条链 |
| `agent-ap-metrics.c`、`agent-ap-metrics.h` | 沙箱，preload | 子节点终端数的替身应答：一帧 `0x800b` + `/run/zte-mesh/ap-sta` → 一帧 `0x800c`（指定的那几路各回一条 `0x94`，空列表＝本机跑的每一路）。没有表、表超过 30 秒没刷新、`epoch` 在未来、查询指定的 BSSID 一个都不认识 ⇒ 回 0 字节＝不吭声；认得的路数照答，**没测到的那一路不补 0**。自己不开 socket，发回哪儿由 `controller-trace.c` 决定；表的路径是 `ZTE_MESH_AP_STA_FILE`，所以主机测试能自备一张表 |
| `controller-1905.c`、`controller-steer.c` | 沙箱 + 主机测试 | 纯函数解码与组帧：CMDU 头与 TLV 走查、原厂 vendor 容器（0x0b/0x21）、`0x94` 按 BSSID 的 `{BSSID, 终端数}`（拆不出可信的一份就整体返回 0，宁缺毋滥）、`0x93` 的查询解码与 `0x800b`/`0x800c` 的组帧、终端引导三类报文 |
| `controller-packet.c`、`controller-wsc*.c` | 沙箱 + 测试环境 | 合成对端和独立的 M2 校验器（只用公开测试值）；主控侧发 `0x800b` 查询（`controller_ap_metrics_query()`：从 `veth-lab` 发、源取 `br0` 的 AL MAC、发出即不读——应答按那个 MAC 回到 daemon 自己的 socket） |
| `config-bridge-probe.c`、`exec-check.c`、`zte-debugcmd.c` | 测试环境 | 早期桥接测试、加载检查、原厂 `sendcmd` 调试通道客户端 |
| `prepare.py`、`jffs2.py`、`vendor_elf.py`、`schema.py`、`profile_schema.py`、`startup-schema.py`、`bridge_evidence.py`、`cm-profile-evidence.py` | mac | 从固件提取运行环境、按固定哈希审计 ABI、核对表结构与偏移证据 |
| `openwrt/` | 路由器 | init（UCI 映射）、rpcd 插件、LuCI 页面、`wlan-sync`、`sta-collect`（宿主侧量终端数）、`make-runtime.py`、`repack-runtime.py` |

## 4. 固件来源

运行环境只从一个固定的原厂镜像提取，`tools/dev/prepare.py` 校验哈希；换镜像要重新调查并更新固定值，没有跳过校验的选项。

```text
zx279133/vendor-reference/2b5/rootfs.jffs2
  └─ /kmoduletmp/kmodule.img            嵌套 SquashFS（inode 496，4944 个数据节点，19,673,088 字节，无缺口）
       ├─ /bin/multiapd
       ├─ /bin/cspd
       └─ /lib/lib{ieee1905,multiap}{cfapi,cmapi}.so 等
```

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `kmodule.img` | 19673088 | `3fbb2e29fe7a94124aba084217c372d2694c880c8657781c9c641f7d0d1fad46` |
| `bin/multiapd` | 988140 | `a0ba6beeb1eb4e0c900b24d06e9d37e7174f9f640320d12803d7d80c4276280a` |
| `bin/cspd` | 3329124 | `8b8477e2598660653c27d796fcaaa5484f879cd263fb6caa5e0a982b1cee5ac1` |
| `lib/libieee1905cfapi.so` | 26000 | `7ed1fb54aedbb9f346152a171d4ccce03f84e047cb677beb6553067f1fc951c3` |
| `lib/libieee1905cmapi.so` | 48016 | `17ae6ce610ff7999aafeba305721601fbae8138e10d338cc1704f95b7ec06329` |
| `lib/libmultiapcfapi.so` | 18148 | `e11166d71f6d5ef9cafe80ff946545ac181c73f4d5e82cfac751a0072fe813dd` |
| `lib/libmultiapcmapi.so` | 59544 | `bf47d0b05d059aa85bb92fbc7e2f1e4502832eb35d3bf394ed44f2db9854be75` |

- `zx279133/vendor-reference/2b5/cspd`（3,106,388 字节，`eb7062b9…`）是**另一个版本**，来源不明，地址和 ABI 不能套到镜像里的
  程序上。表结构还原以镜像内的 `cspd` 为准（`tools/dev/prepare.py --analysis-cspd` 提取到 `out/analysis/cspd`，0600，不进运行环境）。
- 中兴原厂路由器 Z506 的 `multiapd`/`libcmapi.so` 与 SR1010 的也不是同一个构建：控制器行为读 SR1010 的二进制，子路由能否接收读
  子路由自己的。

`multiapd` 保留了大量符号（IDA 识别 2628 个函数）。定位协议入口时用到的地址（链接地址，属于上表哈希的 `multiapd`）：

| 地址 | 符号 | 说明 |
| --- | --- | --- |
| `0x13964` | `main` | `DBShmCliInit`、`OSSInitConfig`、`OSSStart` |
| `0x655b8` / `0x65d2c` / `0x66b0c` | `baseCreateSockfd` / `baseBindSocketToBr0` / `I5MessagePacketSend` | AF_PACKET + `0x893a`；按名字找 `br0`（`controller-route.c` 适配接口身份）；`sendto` |
| `0x414dc` / `0x60070` | `master_init` / `slave_init` | 控制器初始化与控制消息注册；Agent 侧（`EnMasterOnly=1` 时跳过） |
| `0x6f758` / `0x6edd0` | `acdsRcvApAutoConfigSearch` / `_acdsRcvApAutoConfigWSCM1` | Search → Response（含设备过滤）；WSC M1 处理 |
| `0x7ca90` / `0x464ac` | `acdsBuildVendorMsg` / `multiap_get_vendor_oui` | 私有消息构造（按 msgtype 找回调，加 6 字节厂商头）；OUI `00 19 c6` |
| `0x6a344` / `0x3be98` | `baseVendorTypeExtract` / `I5TlvVendorP1905ComCfgSyncInsert` | 按设备身份维护通信许可（[FINDINGS](FINDINGS.md) 第 4 节）；厂商 TLV `0x60` |

## 5. ABI 与沙箱约束

- `multiapd` 和原厂库是 ARM EABI5 soft-float（`e_flags=0x05000200`），解释器 `/lib/ld-linux.so.3`，glibc 2.26。不能注入 AArch64、
  musl 或 hard-float 的库；preload 库和 ARM32 程序用 `arm-linux-gnueabi-gcc -mfloat-abi=soft` 编，直接链接原厂 glibc，
  `make-runtime.py` 再审计强符号与 GNU 符号版本。
- 启动器（`ns-run` / `mesh-search`）是本机 CPU 的机器码，与上面那套 ARM32 无关。它和运行环境在同一个包里（`zte-mesh`，`arch: all`）：
  一个 apk 带 aarch64 / armv7 / x86_64 各一份 `-static` 构建，放在 `/usr/libexec/zte-mesh/arch/<cpu>/`，外层那两个名字是同一份
  shell 入口——按 `$0` 的最后一截选程序、按 `uname -m` 选目录，选中就 `exec`（换程序不换 pid，procd 与 `attach` 的
  `sandbox_pid()` 盯的都是它）。以前它是第二个包（`zte-mesh-launcher`），为的是"一个 Makefile 只给一个 `PKGARCH`"；两边都成了
  `arch: all` 之后那句话说不到什么了，剩下的只是两次事务——升级时先换 `zte-mesh` 还是先换 launcher，中间那几秒服务已经按新脚本重启过一次。
  **换掉的还有装机时的架构判定**：以前那句 `DEPENDS:=@(TARGET_zte||TARGET_qualcommax)` 在一台不匹配的
  机器上会让整串 `apk add` 一起失败（apk 求解全有或全无），现在什么机器都收，"这台不在包里"由入口脚本自己说，
  `zte-mesh.init` 在把实例交给 procd 之前问一次 `--launcher-check`——**只有 127 算这一问的答案**，其余非零另说一行。
  布局、映射表与构建脚本见 [LAB](LAB.md) 第 4.3 节。
- 内核要 `CONFIG_COMPAT`（AArch32 用户态）、SysV IPC（原厂数据库是 SysV 共享内存）与 raw socket（`CONFIG_PACKET`，
  1905 帧由 `multiapd` 在用户态自己收）。这三条原来是根目录那份 `kernel.fragment`，零引用、没被任何构建吃过，删掉并把话写在这里。
- 私有根里保留原厂 loader 和 glibc，不碰 OpenWrt 的 `/lib`；原厂进程看不到 MTD、厂商设备和宿主配置。
- chroot 和 preload 都不是完整的安全边界。沙箱靠命名空间：私有 network/IPC/mount，宿主可写的只有 `/run/zte-mesh`，
  只读字符设备只有内核 urandom。
- `SSEND`/`ASEND` 区分内部任务和外部服务，有同步应答语义，不能一律忽略。只替身确实发给 `cspd` 而控制器需要应答的消息
  （例如启动计数 `0xa96e`），其余照原样走原厂 OSS。
- 外部进程不能直接调 `ASEND/SSEND`（需要 OSS 身份）：`zte-meshctl` 用 `liboss_pub.so` + `InitAppComm`，由服务 fork/exec。
- 守护进程环境里只能有一个变量（`ZTE_MESH_START_COUNT`），多一个 `setenv` 会让原厂 ld.so 断言失败（exit 127）。
  要给 preload 库传状态，经数据库。
- 原厂程序带旧 OpenSSL（1.0.0），要考虑安全维护和再分发许可。

## 6. 为什么选这条路线

| 路线 | 结论 |
| --- | --- |
| 原厂 `multiapd` + 兼容运行环境 + 垫片 | 采用。中兴子节点的准入、私有 TLV、M2 内容都在原厂代码里，实测逐字节一致，协议层不用维护 |
| 开源 EasyMesh 栈（prplMesh）+ 中兴扩展 | 没走：中兴子节点有一串身份、市场码、私有 TLV 的准入门槛（[FINDINGS](FINDINGS.md) 第 4 节），要逐项复刻 |
| 自写轻量 1905/中兴控制器 | 没走：等于重写原厂协议和 WSC，维护和安全测试成本最高 |

垫片的边界（仍然适用）：只覆盖**平台边界**（数据库、接口身份、少数外部服务消息），保留协议引擎和 OSS 内部调度，
不写"全部返回成功"的空函数库；没有本地 Wi-Fi 不等于没有无线配置——控制器仍要远端 AP 的 SSID、密码、安全模式和频段配置，
这些是逻辑配置，不伪造本地无线能力；同一 LAN 上不能有两个 1905 控制器。
