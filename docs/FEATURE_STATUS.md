# 中兴 mesh 功能清单、完成度与推进顺序

> **状态（2026-10-31）：** 主控侧在真子路由（ZXHN Z506，原厂固件）上验证；子路由侧在实验室与两台实验机上验证。
> 本表是唯一的功能总账：每完成一项就在"状态"列打勾并把证据补上，**不要**在别的文档里另起一份清单。
> 协议与逆向细节见 [CONTROLLER_CAPABILITIES](CONTROLLER_CAPABILITIES.md)、[REAL_CHILD](REAL_CHILD.md)、
> [research/AGENT_PROBE](research/AGENT_PROBE.md)；部署与测试见 [DEVELOPMENT](DEVELOPMENT.md)。

## 实验机

| 代号 | 机器 | 架构 / 系统 | 角色 |
|---|---|---|---|
| **VM** | `192.168.1.20`，VMware 里的 ImmortalWrt | x86_64，25.12.2 r38135，内核 6.12.103，**没有无线网卡** | `master`（测 `main` 时临时切） |
| **ax3600** | `192.168.1.1`，小米 AX3600 刷 ImmortalWrt（非中兴硬件） | aarch64 cortex-a53，同上 | `slave`（`radio1`=5G、`radio2`=2.4G、`radio0` 空卡） |

两台都在 `192.168.1.0/24`，root 密码 `root`，公钥已装；都没有 `tcpdump`（`apk add tcpdump-mini` 可装），
dropbear 没有 sftp（传文件用 `ssh A 'cat f' | ssh B 'cat > f'`）。

**当前轮次的产物**：`main` 上 run 41181993285，`zte-mesh-apk` 3.86 MB，拆包后四个包。**这一轮还没装上两台机器**——
下表"实验机"的证据来自上一次装机（拆包前三个包 + 手工补的 `ns-run`/`db-engine-probe`）；r10 装机验收待做。
装法：ax3600 装 `zte-mesh` + `zte-mesh-launcher`；VM 是 x86_64，只能装 `arch: all` 的 `zte-mesh`
（`launcher` 在它上面报 `uninstallable`，属预期）。都加 `--allow-untrusted`，`/etc/config/zte_mesh` 不会被覆盖。

## 图例

| 记号 | 含义 |
|---|---|
| ✅ | 已实现，**真机**验证过 |
| ☑️ | 已实现，只在实验室 / 测试桩上验证（未上真机，或对端固件不支持） |
| ⬜ | 未做 |
| ➖ | 做不到：对端没有接收代码或没有下发通道（不是我们没写） |

难度：**1** 改几行、**2** 一天内可验证、**3** 需要新逻辑或新测试面、**4** 需要真机驱动侧配合、**5** 需要逆向或换硬件。

## A. 主控（本机是主路由）

| # | 功能 | 状态 | 证据 | 难度 |
|---|---|---|---|---|
| A1 | 原厂 `multiapd` 在私有命名空间里跑（IPC/net/mount、veth 到 `br-lan`） | ✅ | `ns-run` + `attach`，真路由器 + 真 procd | — |
| A2 | 发现 / 拓扑 / 链路指标 / 高层数据（CMDU 0、1、2、3、5、6、7、13、14） | ✅ | REAL_CHILD §1 | — |
| A3 | AP-Autoconfig + WSC M1/M2（真 DH、HMAC、Key Wrap） | ✅ | 子路由 `config M2[0]` | — |
| A4 | 配置变更后 Renew（热更新，不重启服务） | ✅ | 改配置约 4 秒生效 | — |
| A5 | 主 SSID / 密码 / 加密 / 隐藏 | ✅ | 2.4G、5G 均读回 | — |
| A6 | 多 BSS（访客、备用、隐藏回程，槽位 1–3）名称 / 密码 / 开关 | ✅ | REAL_CHILD §1 | — |
| A7 | BeaconInterval（含热更新） | ✅ | 120 → 读回 120 | — |
| A8 | ACL 名单 + 每 BSS 策略（关 / 拦 / 只放行） | ✅ | 一加手机被踢并拒绝重连；`allow` 与 5G 驱动读回未验证 | — |
| A9 | 信道与发射功率（`0x8006`） | ✅ | 两个频段验证并还原 | — |
| A10 | 重启子节点 / 指示灯开关 | ✅ | REPLAY_RESULTS 16.1 | — |
| A11 | 访客限时、Wi-Fi 定时关闭 | ✅ | `scope=guest` 真机；`scope=all` 仅实验室 | — |
| A12 | 自组网 V2（`mesh.v2`）与市场码门槛 | ✅ | 两种取值都正常入网 | — |
| A13 | 控制器角色 `master` / `auto`（含自适应选举替身） | ✅ | 第 40 轮 | — |
| A14 | MLO 开关下发（`0200fe`） | ☑️ | 线上帧逐字节一致；Z506 无该字段，等于无操作 | — |
| A15 | Wi-Fi 5 兼容标志（`ForceWiFi5`） | ☑️ | 线上值正确；Z506 不认 | — |
| A16 | 漫游 / 频段引导阈值下发 | ☑️ | 能发；Z506 固件没有解析代码 | — |
| A17 | 引导（BTM）/ 封锁 / 踢终端 | ⬜ | `0x8014` 等，接口已在 libcmapi | 3 |
| A18 | 拓扑页：回程 RSSI、树状图、每 AP 终端列表、固件版本 | ⬜ | 现在只有节点清单与动作 | 3 |
| A19 | 服务配置校验：`zte-mesh-timer` / ACL / 槽位连续性的错误定位 | ☑️ | init 79 项 | — |
| A20 | **界面中英双语**：文案全走 `_()`，`po/zh_Hans` 与源码逐条对齐 | ✅ | i18n 12 项；两台随包装出 `zte-mesh.zh-cn.lmo`（11632 B），`/cgi-bin/luci/admin/translations/zh-cn` 返回中文 | — |

## B. 子路由（本机是子节点）

| # | 功能 | 状态 | 证据 | 难度 |
|---|---|---|---|---|
| B1 | agent 模式启动（`Mode=1` / `SetRoleEnable=0` / `EnMasterOnly=0`） | ☑️ | 实验室：三个值均读回 | — |
| B2 | 发 M1、收 M2、凭据落 `WLANCfg` / `WLANPSK` | ☑️ | 两个频段各一份，`m2_match=1` | — |
| B3 | setter 兜底（配置管理器返回 -3 时读回校验再报 0） | ☑️ | `libzte_agent_mm.so`，沙箱专用 | — |
| B4 | 库 → 计划的发布（`agent-sync` → `/run/zte-mesh/wlan-plan`） | ☑️ | `published=1 rows=2` | — |
| B5 | 计划 → `/etc/config/wireless`（`wlan-sync`，幂等、只写真变化；plan 启用某 band 时把关着的 radio 打开） | ✅ | 86 项测试；ax3600 人为 `disabled=1` 后重启服务，日志 `switched on ... applied`，空卡 `radio0` 未被碰 | — |
| B6 | 回程 BSS：`MapAccessAttr&64` → `multi_ap=2` 独立段，主 SSID `multi_ap=1` | ☑️ | 86 项测试含开关双向复原 | — |
| B7 | UCI / init / LuCI 暴露 `mesh_role` + `backhaul` | ✅ | init 79、rpcd 18；两台 `uci` 写的三个角色都被 init 接受 | — |
| B8 | **子路由免本地 profile**：没有配置时沿用本机正在广播的网络 | ☑️ | init `role-slave-borrow` | — |
| B9 | **LuCI 按角色只显示有效设置**（子路由隐藏 Wi-Fi/漫游/额外网络/子路由清单） | ☑️ | `test_luci.js`（主控 31 项 / 子路由 4 项） | — |
| B10 | rpcd `status` 返回角色 | ✅ | rpcd 18 项；两台各自读到 `master`/`main`/`slave` | — |
| B11 | 子路由侧应用额外槽位（slot 1–3 现在只记日志） | ⬜ | `wlan-sync` 明确跳过 | 3 |
| B12 | **应用结果回传**：`wlan-sync.status` + `status.sync` + 状态页行 | ☑️ | wlan-sync 86 项、rpcd 18 项 | — |
| B13 | 无线回程的数据面：本机作为 backhaul STA 去关联主控的回程 BSS | ⬜ | 只做了 AP 侧 | 4 |
| B14 | 真机：本机当子路由，接真主控（有线回程） | ✅ | 两台实验机打通。抓包确认**子路由自己会发** `0x0007` Search、主控回 `0x0008` 带 WSC；原判断"子路由被 cspd 门控"是错的，真因是 C6。r7 后主控改 SSID，子路由 `wlan-plan` 与 `iw dev` 跟着变成 `MASTERTEST`/`MASTERTEST-5G` | — |
| B15 | `CmWLANMapSsidCfgSync2Repeater` 对齐（libcmapi 内部调用，替身命中不到） | ⬜ | 沙箱里是 no-op | 5 |
| B16 | `CurHop` 在子路由侧的真实更新 | ⬜ | 实验室用 `-2` 顶替 | 2 |
| B17 | **三角色**：`master` 只控制 / `main` = 主控 + 本机落地 / `slave` 只被控；`main` 的 plan 由 init 写 | ✅ | init 79；VM 切 `main` 后 init 写出 `wlan-plan`（`rows=2`）、`wlan-sync` 在跑、日志 `no usable 2g radio; left alone`；切回 `master` 后 plan 与实例一起消失（一个 plan 只有一个生产者） | — |
| B18 | **cspd 替身**：控制器侧自己发 `0x0007` Search、并代答子路由的 Search（`0x0008`） | ✅ | `enroller.c` → `/usr/libexec/zte-mesh/mesh-search`，init 在非 `slave` 角色下多起一个 `search` 实例，带 `--al-mac 02:00:00:00:19:<al_last>`（子路由的 M1 发给控制器 AL MAC）。`--search` 每次启动只发**一帧**，所以周期就是"多久收一次新子节点"，已从 `86400` 改成 `300`（最坏等 5 分钟）；实测不抖：`plan applied nothing new`、`daemon_restarts=0`。装 r6 后真机自动入网，不再需要手工 enroller | — |

## C. 打包与分发

| # | 功能 | 状态 | 证据 | 难度 |
|---|---|---|---|---|
| C1 | 单个 apk（runtime + init + wlan-sync + rpcd + LuCI），4.1 MB | ✅ | 真路由器上 procd/rpcd 全通 | — |
| C2 | 拆包：`zte-mesh`（`arch: all`）+ `zte-mesh-launcher`（aarch64，只装 `ns-run`/`mesh-search`） | ✅ | **只做了"让 x86_64 能直装"这一半**。必须两个目录：`package-pack.mk` 的 `Architecture` 读 Makefile 级全局 `PKGARCH`，一个 Makefile 给不了两个架构。VM 现在 `apk add zte-mesh` 直装且不覆盖自编的 `ns-run`。原方案的 controller/agent 拆分仍未做 | 1 |
| C3 | 统一行尾（`.gitattributes`，`* text=auto eol=lf`） | ⬜ | 工作区 CRLF，测试要转 LF 才能跑 | 1 |
| C4 | **中文翻译包随 CI 构建**：`luci-i18n-zte-mesh-zh-cn` 之前只配 `=m` 没编译，中文从不产出 | ✅ | 两台装完后 `zte-mesh.zh-cn.lmo` 就是包带来的 | — |
| C5 | **包里的 `ns-run` 与 init 不同步**：tarball 里是旧版，不认 `ZTE_MESH_QEMU`/`ZTE_MESH_EMULATED_LAB`，无 `CONFIG_COMPAT` 的 aarch64 上 `execv` 直接 `errno=8` | ✅ | 修法是消除漂移本身：tarball 额外带 `ns-run.c`/`controller-net.c`/`enroller.c`，`Build/Compile` 用 `$(TARGET_CC)` 现编；`test_runtime.py` 比对源码副本，重打包漏了就在本机失败 | — |
| C6 | **tarball 里的 `db-engine-probe` 冻结在打包那天**：ARM32 半边只能 `-nostdlib` 对着原厂 glibc 2.26 编，包里编不了；冻住的那份不认 `mesh.role=slave` → 子路由的 controller 实例每 5 秒退一次 | ✅ | 换重编副本（193364 B，sha256 `7ff631a7…`）后 slave 角色 3/3 实例全起；`test_runtime.py` 断言包里那份必须带 `config-reject` 与 `wlan-plan` | — |
| C7 | **全新装包后服务起不来**：`qemu-arm-static` 不在包里，而 `CONFIG_COMPAT` 探测在官方固件上必然误判（不导出 `/proc/config.gz` → `grep -c` 返回 0 → 一律走模拟器） | ✅ | 改成"没有答案不等于不支持"：读不到 config 就 exec 一次 `$ROOT/lib/ld-linux.so.3` 看它有没有打印 usage。模拟器**仍不入包**（取舍，不是待办），判定表与三条出路见 [DEVELOPMENT](DEVELOPMENT.md) 第 11.2 节 | 1 |

## D. 没有通道的（换子路由固件才谈得上）

| # | 功能 | 原因 |
|---|---|---|
| D1 | 带宽 20/40/80/160 | Z506 不写 `BandWidth`，也不关"自动信道" |
| D2 | 访客限速 | 1905 里没有这个字段 |
| D3 | 信道扫描 / CAC | Z506 无接收代码 |
| D4 | VBSS（虚拟 BSS 创建/销毁/迁移） | Z506 无 |
| D5 | VLAN / 流量分离 / Profile-2 | Z506 无 |
| D6 | STA 绑定 | Z506 无 |
| D7 | 802.11r/BTM 踢人（部分） | Z506 固件不支持 FT |

## 推进顺序（只做下一项，做完打勾再往下）

| 顺序 | 项 | 难度 | 说明 |
|---|---|---|---|
| — | B8 / B9 / B10 / B12 / A20 / B5 / B7 / B17 / C4 / C5 / C6 / C7 | — | **已完成**（2026-10-31） |
| 1 | r10 四包装机验收 | 0 | 产物已在手，两台换上即可 |
| 2 | ax3600 刷 `CONFIG_COMPAT=y` 固件 | 1 | 彻底摆脱手工模拟器，`.github/workflows/build-firmware.yml` |
| 3 | C3 统一行尾 | 1 | 影响整个仓库，**需要单独确认** |
| 4 | B16 `CurHop` 真机确认 | 2 | 需要真机 |
| 5 | B11 额外槽位在子路由侧应用 | 3 | 先要把 plan 的槽位与 hostapd 段对上，且有 A6 真机作参照 |
| 6 | A18 拓扑页 | 3 | 需要 RSSI、终端数先落到 `state.json` |
| 7 | A17 引导 / 封锁 / 踢终端 | 3 | 接口已存在，缺策略与 UI |
| 8 | B13 无线回程 STA | 4 | 要真机看 `4addr`，且与 wpad 变体绑定 |
| 9 | B15 `CmWLANMapSsidCfgSync2Repeater` | 5 | 逆向或真机 strace，前 8 项都过了才值得动 |
