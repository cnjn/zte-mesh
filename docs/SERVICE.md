# B.4：长期运行的 Controller 服务模式、节点清单与配置下发

本文只管 **UCI / CM / 热更新的映射**：哪个选项翻成哪个服务键、哪类变化走通知、哪类要重启、状态里多出哪些字段。状态与"做没做"见 [FEATURE_STATUS](FEATURE_STATUS.md)，上机待办与排期见 [PLAN](PLAN.md) 第 5 节，构建与测试命令加各套测试的条数基线见 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节，原理与源文件职责见 [ARCHITECTURE](ARCHITECTURE.md)。

## 已成立的事实

- **节点清单**：`P1905DeviceInfo`（11 字段/788 字节/1024 行，主键 ViewName+MacAddr）从 `cspd` 静态恢复；真实 `multiapd` 收到对端后自己写入行（AL MAC、Active、WSC M1 带来的 ModelName）。`controller_read_nodes()` 只读导出，不含序列号/ProductClass。
- **服务模式** `ns-run … --controller-service`（`controller-service.c`）：没有限时，配置由宿主写 `/run/zte-mesh/config`（0600，严格 key=value），状态每秒写 `/run/zte-mesh/state.json`（`/run/zte-mesh` 是沙箱里唯一可写的宿主路径）。
- **配置下发链路**：改 profile → 事件 `0xa93e` 发给 `multiapd.acd.acd` → 原厂 `_handleWlanRenew` → 真 daemon 发 AP-autoconfiguration Renew（CMDU 10）。`CmMultiAPWlanRenew` 本来发给 `cspd.cspd.multiapd_mgr`（事件 `0x9c8a`），我们不跑 cspd，所以 helper 直接发 `0xa93e`（8 字节 `{band,0…}`）。
- **外部进程不能调 CM**：`ASEND/SSEND` 需要 OSS 身份，且要用客户端库 `liboss_pub.so` + `InitAppComm`（工厂 `diagget` 的做法），不能用 daemon 的 `liboss.so`；从监督进程直接调会崩。所以 `zte-meshctl`（ARM32）链 `liboss_pub`，由服务 fork/exec。
- M2 的 Authenticator/Key Wrap/profile 由独立校验器通过（`wsc_cryptographic_verified` 进报告）；标准模式（`VersionMode` bit1）下节点 `ZTEDevFlag=0`，型号来自 WSC M1。

## MLO 开关与跨设备模块消息（`0200fe`）

- 配置键 `mlo.enable=0|1`（默认 1，与原厂 `WLANSetting.MLOEnable=1` 一致），启动时写库，可热更新；`wifi.dual_band_sync` 同时写 `WLANSetting.DualBandSync`（只是启动值）。
- 原厂 `cspd.wlan_manage.wlan_mgr` 在设备上线（及 MLO 开关变化）时，经 `CfIeee1905TunnelToDevice` **异步**发事件 `0xa97e` 给 `multiapd.topo.topo`，daemon 把它作为厂商 TLV `0200fe` 发给子节点：体 = `{u16 BE 36, 名字[36]="cspd.wlan_manage.wlan_mgr", u32 BE 1, u32 BE 4, u32 BE 值=MLOEnable}`。服务在 ZTE 子节点在线满 15 秒后调 `zte-meshctl wlan-mgr <AL MAC> <0|1>`（每个子节点各自计时，最多 8 个，每个最多重试 4 次）。**`0200fe` 必须用 `ASEND`**（`ASEND(0xa97e)`）：同步 `SSEND` 走请求表，表里不含 `0xa97e`，**返回 0 但线上没有帧**。
- 原厂入网时发 3 条，shim 每个会话发 1 条；Z506 的 `WLANSetting` 没有 `MLOEnable` 字段，所以对它是无操作。启动时还要注册三张空视图/小表（`MultiAPRoamCatonCfg`、`WLANSetting`、`MultiAPCacheMac`），否则第一条 `PolicyConfigReq` 的厂商体会只剩 1 字节。

## CM 通知层：策略类设置的热更新

原厂 CM 设置函数（`libmultiapcmapi.so`）写库后 `CmSendMsg2MM("multiapd.map.master", 0x2401, 消息号, name[32], 无负载)`，
即**同步** `SSEND(0x2401, {u32 消息号, char name[32], u32 0})`，主控在 `master_handle_synmsg` 里按 SynSet 表分发。
直接写库（`sendcmd 1 DB set`）不会发它，`multiapd` 既不重读也不推送。

| 消息号 | 谁发 | 处理 | 线上效果 |
|---|---|---|---|
| `0x10206` | `SetRoamCfg`、`SetDomainCfg`、`SetBSCfg`、`SetBSEnable` | `_master_set_doamin_cfg` | 每个子节点一个 `PolicyConfigReq`：`MetricRptPolicy` + 厂商 `4a[12] 4d[1] 60[2]`（83 字节） |
| `0x10205` | `SetMasterCfg`、`SetMultiAPCfg` | `_master_set_master_cfg` | 纯厂商 TLV 的 `PolicyConfigReq`：`4c[2] 5a[1] 4d[1] 60[2]`（52 字节） |
| `0x10208` | `SetBtmReqModeCfg` | `_master_set_btm_req_mode` | 无帧（只更新判断参数） |

服务里的实现：`zte-meshctl notify-master <id>...`（只允许 `0x10205`/`0x10206`/`0x10208`，可多个、同进程按序发；原厂网页路径两条通知间隔 12 ms）；`mesh-policies.c` 的 `mesh_policies_live_*()` 在池锁下写全部 8 个 `MULTIAPDOMAIN` 行或 Master 行 0。Domain 字段变 → `0x10206`，Master 字段变 → `0x10205`，两者都变按原厂顺序先 06 再 05；启动时不发。

热更新范围：已有序号 K 的 ssid/key/hidden/beacon_type、主 SSID 的 `hidden`/`beacon_type`、`mlo.enable`、`beacon_interval`、信道/功率、ACL、`wifi.dual_band_sync`、`roam.*`、`band_steer.*`、`roam.force`、序号的 `enabled`；WLAN 类变化**只 Renew 变化了的频段**。键的出现/消失（无→有、有→无）要重启服务（`policy_keys_changed`）。

## 守护进程重启：mesh 级变化

原厂的"mesh 级 Renew"其实是 cspd 的 `multiapd_mgr` 重启 `multiapd`（`CmMultiApTopoSetP1905ComCfg` 写库后无条件 `ASEND(0xa93d)`；`CmMultiAPSetMultiAPCfg` 在 `Mode`/`Enable`/`VersionMode` 变化时 `ASEND(0xa93c)`）；两个频段的 Renew 和 6 到 40 秒的拓扑抖动是新进程起来的副作用。

服务里（只做了 `mesh.v2`）：`mesh.v2` 改动 → `v2_update`（池锁下写 `P1905ComCfg.Enable` 并读回）→ `daemon_respawn`：SIGKILL 旧进程、`waitpid`、探测数据库池锁与订阅池锁（被杀的进程可能握着锁，失败则服务退出交给 init）、在同一数据库上 fork/exec。数据库和订阅池属于监督进程，不丢；`state.json` 的 `daemon_pid` 换新、`daemon_restarts` 加 1。写库或读回失败退回 `restart_required`。

- 重启后清掉推送记录，`0200fe`、信道/功率要等 `P1905DevNum >= 2` 再计 15 秒：新进程起来时库里 `Active` 是旧进程留下的 1，抖动期推出去的消息会被丢弃。
- 原厂重启后第一对 Renew 由 ACD 的 3 秒定时器触发：`acdsCheckMultiapdRestart` 向 `cspd` 取启动次数（`SSEND(0xa96e)`）。shim 里没有 cspd，`controller-route.c` 拦截那一条，按 `ZTE_MESH_START_COUNT` 回答（首次开机为 1，冷启动行为不变）；守护进程每次启动都按"重启"计，所以服务单独重启时已入网的子节点会在约 40 到 60 秒内重新入网。**不要自己发 ACD 事件 2**：守护进程收到第一个 1905 帧时会自己发，提前发会浪费一次性的 `g_AcdsRenewStat`。
- 真实路由器（15 次重启）：子节点掉出拓扑、`TopoResponse` 不带 `1905Nbr`、8 到 58 秒后重新入网；`PolicyConfigReq` 的 `60[2]` 跟着 `mesh.v2` 变。新进程要收到子节点的下一帧才往外发，所以重新入网跟着子节点的周期帧走。
- 仍要重启服务的：`version_mode`，以及 `roam.*` / `band_steer.*` / `roam.force` 这些键的**出现或消失**（`policy_keys_changed`）。增删序号不重启服务，但会重启守护进程（ACD 只在启动时按 `P1905AcdBssCfg` 建每 BSS 列表）；已有序号的 `enabled` 是热更新（每次 Renew 从 `WLANCfg` 读回）。

## 设备角色与 CurHop

原厂"自适应" = `Mode 0` + `SetRoleEnable 1` → 守护进程重启；cspd 的 `cooperator_mgr` 判定路由模式 → ACD 的 `_autoSetRole(2)` 写 `Mode 2`、`SetRoleEnable 1`，再要求第二次重启（约 3 秒后）。

- 可选键 `mesh.role=master|auto|slave`（UCI 侧的新取值见"设备角色"）：`master`/`auto` 写 `Mode 2` 和 `SetRoleEnable`（0/1）；`slave` 写 `Mode 1 / SetRoleEnable 0 / EnMasterOnly 0 / CurHop -2`，并在运行时把主控下发的 Wi-Fi 配置发布成 `/run/zte-mesh/wlan-plan`（`agent-sync.c`），由沙箱外的 `wlan-sync` 写进 `/etc/config/wireless`。
- plan 有两个生产者，按角色互斥：子路由由服务写；`main`（主路由）由 init 的 `write_config` 直接写（`write_local_plan`，格式同）；仅主控不写，还会删掉旧文件。`wlan-sync` 不关心是谁写的。
- 热更新 `master→auto`：写 `SetRoleEnable 1`、`Mode 0`，重启守护进程；约 3 秒后写 `Mode 2` 再重启一次——**第二步是监督进程替 cspd 的 `cooperator_mgr` 做的模拟**（守护进程自己不会把 Mode 写回 2）。`auto→master` 只清 `SetRoleEnable`，不重启。
- `CurHop` 恒为 2：`EnMasterOnly=1` 让原厂会把它重置成 -2 的 `slave_init` 跳过；线上 `HigherLayerResponse` 子 TLV `0x43` 与原厂一致（原厂的 -2 只是库里几秒的瞬态）。
- 没做：Slave 侧、桥接模式判断、ACD 自己的 1905 选举协议。

## UCI 与 LuCI 映射

`zte-mesh.init` 的 `write_config` 把 `/etc/config/zte_mesh` 翻成服务配置（`/run/zte-mesh/config`，0600，原子替换）；`luci-app-zte-mesh` 的 `zte-mesh.js` 是对应表单。init 只为让日志说出哪个选项不对而检查，服务自己仍会再校验整份文件；任何一项不合法都不启动/不替换配置（日志 `zte-mesh: <选项> ...; not starting`，**不记录 SSID 和密钥的值**）。

主区段 `config zte_mesh 'main'`（没写的选项不产生对应的服务键，路由器保持固定默认值）：

| UCI 选项 | 服务键 | 说明 |
|---|---|---|
| `enabled`、`bridge` | `version_mode` 固定 128 | `VersionMode` 是 SR1010 原厂值（Z506 自己是 0，应用 M2 时跟随控制器）；原厂界面没有这个设置。旧的 `mode` 选项已删除，写了也被忽略 |
| `wifi_24g_enabled` / `wifi_5g_enabled`、`wifi_24g_ssid` / `_key` | `radio0/1.*` | 2.4G 名称和密钥必填；布尔接受 1/yes/on/true 与 0/no/off/false；`enabled=0` 会真的关掉子节点该频段的主 SSID |
| `wifi_dual_band_sync` | `wifi.dual_band_sync` | 1：5G 用 2.4G 的名称、密码、安全方式、隐藏与 ACL 策略，`wifi_5g_*` 被忽略且 LuCI 不显示 |
| `wifi_5g_ssid`、`wifi_5g_key` | `radio1.ssid/key`（sync 关时） | 可留空：5G 名称 = 2.4G 名称 + `wifi_5g_suffix`，密钥同 2.4G |
| `wifi_5g_suffix` | `wifi.essid_suffix` | 1 到 8 个可打印字符，默认 `-5G`；派生名超 31 字节被拒 |
| `wifi_*_hidden` / `_security` | `radioN.hidden` / `beacon_type` | `none`=0、`psk2`=3、`psk-mixed`=6、`sae`=10、`sae-mixed`=12；`none` 会让真实网络变成开放网络 |
| `wifi_*_beacon_interval`、`_channel`、`_tx_power`、`_acl_policy` | `radioN.*` | 范围同 REAL_CHILD 第 3 节（信道按频段白名单，0 = 自动） |
| `mesh_compatible`、`mesh_role`、`mlo_enable` | `mesh.v2`、`mesh.role`、`mlo.enable` | `mesh_compatible` 即原厂 MESH 开关（页面标签「MESH 兼容」）：开 = 接受任何子路由（默认），关 = 只接受市场码相同的中兴原厂路由器。服务键 `mesh.v2` 问的是相反的问题（`P1905ComCfg.Enable` = 严格的中兴准入），所以**只有 init 写这一行时反一次**，别处不再翻。旧的 UCI 选项 `mesh_v2` 已删除，写了也被忽略 |
| `backhaul` | **不写进服务配置**：`wlan-sync` 自己读这个 UCI 选项 | 回程模式，`off`（默认）或 `multi_ap`；其余取值 `write_config` 拒绝（`multi-ap` 会被当成 `off`，等于一个"设了却不动"的开关）。按角色写成两端之一，见下面"设备角色"一节 |
| `extra_slots`、`wifi_2g`、`wifi_5g` | **不写进服务配置**：同上，`wlan-sync` 自己读 | 额外网络开关 + 两块射频的指定（`wifi-device` 段名，如 `radio1`；只在"一个频段有两块射频"时需要）。页面上和 `backhaul` 同一批，只给跑 applier 的两个角色；改任何一项都会重启 applier，见下 |
| `band_steer*`、`roam*` | `band_steer.*`、`roam.*` | 两个频段的阈值必须成对给出 |
| `ban_minutes` | `ban.minutes` | 0..1440 分钟，0 = 直到删除记录；对所有封禁生效，是"从第一次下发算起"的绝对时长 |

### 设备角色（两个开关）

原厂那台"主路由"既当主控又自己发 Wi-Fi；SR1010 这类刷 OpenWrt 的机器大多是有线网关、没有无线。以前用一个三值选项
表达，现在拆成两个各管一件事的开关——`mesh.role` 只有 `master`/`slave` 两个值发给服务：

| `mesh_role` | `apply_local` | 下发配置 | 应用到本机 `/etc/config/wireless` | 写 `mesh.role` |
|---|---|---|---|---|
| `master`（默认，未设置也等于它） | `0`（默认） | 是 | **不动** | `master` |
| `master` | `1` | 是 | **是**（plan 由 `write_config` 写） | `master` |
| `slave` | 不起作用 | 否 | 是（plan 由服务从 M2 读出来写） | `slave` |

`main` 和 `auto` 是"主控且自己广播"的旧名字，**仍被读**，等于 `master` + `apply_local=1`（显式写了 `apply_local` 时以选项为准）；daemon 一侧不再收到 `auto`——那是原厂"自适应"三件套（`SetRoleEnable=1` + `Mode 0`，三秒后才由 `role_settle_at` 写成 `Mode 2`），沙箱里没有 cspd、这场选举没有对手，而真正的风险是一台中兴原厂路由器在那三秒里当选主控、拓扑反过来。C 侧的 `CONTROLLER_ROLE_AUTO` 分支与 3 秒结算留着不删：`/run/zte-mesh/config` 是升级前写的旧文件时它仍把角色定在 master，而新写的配置永远走 `master` 那条（`SetRoleEnable=0`、`Mode 2`，从第一帧就是主控）。

`apply_local=0` 的主控会删掉 `wlan-plan`、**不启动** `wlan-sync` 实例，射频存在时日志会明说"本机无线不动"。`mesh_role` 与 `apply_local` 的取值现在都进 `restart.sig`：它们改的是 procd 实例集合（`search`、`wlan-sync`、`sta-collect`），只记键名看不出差别，以前必须手动重启才会生效。

`sta-collect` 只在 `mesh_role=slave` 起（与 `search` 互斥：主控问，子节点答）。它把本机每个 AP 的关联终端数写进 `/run/zte-mesh/ap-sta`，沙箱里的应答读这张表回主控的 AP Metric Query——原厂 agent 那一份数来自驱动经 cspd，沙箱两样都没有，所以没有这张表就没有任何 OpenWrt 子节点报得出终端数（原理见 [ARCHITECTURE](ARCHITECTURE.md) 第 2 节，判据见 [FEATURE_STATUS](FEATURE_STATUS.md) A26）。`stop_service` 连同 `ap-sta` 一起删：一份没人刷新又留着的表会被当成刚测过的数答出去。

这一条循环可以单独停：往同目录的 `collect-pause` 写点什么都行（`touch /run/zte-mesh/collect-pause`），脚本不再重写表，**表留着、`epoch=` 不再更新时间戳**——30 秒后沙箱里的应答就不回主控，这正是"这台没人上报终端数"的样子；`rm -f` 那个文件，下一次 5 秒的循环恢复更新时间戳。日志各说一次（`paused by …: leaving … as it is` / `collecting N access point(s) into …`）。为什么不用 procd 停这个实例：`ubus call service delete` 停得掉，但恢复只能走 `/etc/init.d/zte-mesh start`，那一下把守护进程也换了 pid、子节点从拓扑里掉出去十几秒——读回来的是"重启了一次"，不是"采集停了"。

第三个开关 `backhaul`（`off` / `multi_ap`，别的取值 `write_config` 直接拒——`multi-ap` 这种带连字符的写法会被 `wlan-sync` 当成 `off`，页面上一切正常、射频上什么也没建）也归 `wlan-sync` 管，但它落不落得下取决于上面两个：plan 里 `MapAccessAttr & 64` 的那一行**按角色写成两端的某一段**——`mesh_role=slave` 是客户端（`zte_backhaul_sta_<band>`，`mode sta` + `multi_ap 1` + `wds 1`），`master` + `apply_local=1` 是回程 BSS（`zte_backhaul_<band>`，`multi_ap 2`，主 SSID 标 `multi_ap 1`）；`master` + `apply_local=0` 什么都不生成，所以 `start_service` 对这一组合明说一句，别让人以为"回程没生效"。4 地址为什么在客户端要写 `wds` 而不是只写 `multi_ap`，见 [research/AGENT_PROBE](research/AGENT_PROBE.md) §15。换角色时上一端的段会被回收，不会留下一个"页面看着正常、实际不通"的半条回程。

它的取值也进 `restart.sig`，理由和上面两个相反：它不改实例集合，也不改 plan（plan 里那一行一直都在），只改 `wlan-sync` 拿到同一份 plan 时**写什么**——而那个 watcher 只对 plan 的内容起作用（`stamp()` 比的是文件摘要），所以 `off → multi_ap` 保存后若不重启 `wlan-sync`，就要等下一次别的原因引起的重启才生效。签名里只有"跑着 applier 的两个角色"带这个值（`sync_runs`，与 procd 那段用同一个判据）：`apply_local=0` 的主控不把任何配置应用到本机，为一个不起作用的值重启服务是白重启。

`wlan-sync` 直接读 UCI 的另外几项同理，一起进签名：**`extra_slots`**（子节点的 plan 里本来就有额外网络的行，这个开关只决定它们该不该写到射频上——关掉它却不重启，子路由会继续广播一个已经不该存在的 guest）与 **`wifi_2g` / `wifi_5g`**（一个频段有多块同类型射频时指定用哪一块，选出来的设备名不在 plan 里）。四项共用 `sync_runs` 这一个判据：不跑 applier 的角色一个都不带，否则一次保存就是一次没有意义的重启（子路由最长一分钟掉出拓扑）。页面上这四项也在一起（`zte-mesh.js` 的 `child` 或 `local` 三块），两个 pin 填的是 `wifi-device` 段名，情况非法在保存前就被拒——写错的设备名会被 `wlan-sync` 忽略并留一行日志，那正是"设了却不动"的另一种情况。

包的默认配置把能取出厂值的都写上，LuCI 显示同样默认值并标注范围：`mesh_compatible=1`（也就是 `mesh.v2=0`）、`mlo_enable=1`、`wifi_dual_band_sync=1`、`wifi_5g_suffix=-5G`、`wifi_24g_ssid=ImmortalWrt`、安全方式 2.4G `sae-mixed` / 5G `psk-mixed`、信标 100 ms、信道 0、功率 20/23 dBm（子节点最大档 = 出厂值）、`band_steer=1`、引导阈值 -60/-110、漫游阈值 -60/-70、`roam_force=0`、`enabled=1`、`wifi_24g_key=Ztemesh2631!`（清空则服务不启动）。要让子路由保持自己的信道/功率，删掉对应选项。

其余区段：`zte_mesh_client`（最多 8 条，`mac` 必填且同一地址不许重复，`action` 为 `ban`/`kick`，默认 `ban`；按文件顺序转换成 `ban.mac` / `kick.mac`）、`zte_mesh_bss`（额外网络，`band`/`slot`/`enabled`/`ssid`/`key` 必填，同频段序号必须 1..k）、`zte_mesh_timer`（最多 8 条，`start`/`end` 必填，`days`、`scope` 可选；init 把 `date +%z` 换算成 `timer.utc_offset`，**夏令时切换后要重新保存设置**）、`zte_mesh_acl`（最多 16 条，按文件顺序编号）、`zte_mesh_node`（别名，只给 LuCI 节点表）。

重载：`reload_service` 只在"需要重启服务"的东西变了才重启——`version_mode` 变了，或策略键（`mesh.role`、`wifi.dual_band_sync`、`band_steer.*`、`roam.*`、`mlo.enable`）出现/消失（init 把键名存进 `/run/zte-mesh/restart.sig` 比较）；这些键**值**变了不重启，走热更新。签名里另有两类是**按值**记的：`role=`、`local=`（`mesh_role` / `apply_local` 改的是 procd 实例集合）和 applier 自己的四个输入 `backhaul=` / `extra_slots=` / `pin2g=` / `pin5g=`（`wlan-sync` 只对 plan 的内容起作用，改这些必须重启它才看得见；不跑 applier 的角色不带）。`zte_mesh_client` 与 `ban_minutes` **不在** restart.sig 里：服务每秒重读这份配置，状态机自己比对新旧规则——新规则立刻发，删掉的规则补发一条"放行"再丢弃，所以加/删一条封禁不需要重启。

守这些映射的测试都不需要 OpenWrt：`tests/test_init.sh`（busybox ash + UCI 桩）、`node tests/test_luci.js`（表单跑一遍，核对每个 init 读的选项都有表单项）、`tools/dev/dev-config-check.py DIR`（把 init 生成的配置交给真服务，报告接受/拒绝）；**条数基线只写在 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节**，这里不重复数字。

对已有配置的影响：随包模板新增 `option wifi_dual_band_sync '1'`（conffile，已有配置不改）——**没有这一行、又留空 `wifi_5g_ssid` 的旧配置以前拒绝启动，现在会派生 `<2.4G 名称>-5G` 并启动，也就是会改子节点真实的 5G 名称。** 另外 `radio1.ssid/key` 变成可选；init 归一化布尔值并拒绝不可打印字符；带序号的 ACL 规则以前被整份拒绝，现在接受（旧 bug：检查在序号表填好之前执行），但**没有在中兴原厂路由器当子节点时试过序号上的 ACL**。

## 节点管理、拓扑摘要、定时与访客限时

- **节点动作**：宿主往 `/run/zte-mesh/actions/<id>.act`（0600，文件名只用 `[A-Za-z0-9_-]`）写一行 `reboot <mac>` 或 `light <mac> 0|1`；服务每秒取走（读完即删，启动时先清遗留），只对在线且已识别为中兴设备的节点执行（`topology_ready` 之后）。结果进 `state.json` 的 `actions`（最新在前，最多 8 条；`status` 0 已发出、-2 格式错、-3 无此节点、-4 守护进程没就绪）。rpcd `luci.zte-mesh reboot_node {mac}` / `light_node {mac,on}` 写请求并最多等 8 秒（-5 = 服务没运行）。
- `state.json` 的节点多了 `light`（-1 未知）、`light_age`、`stations`（-1 未知）；`sched` 有 `rules`、`off_all`、`off_guest`、`access`（各访客倒计时）。`light` 来自 trace 库对子节点帧的摘要（`/run/zte-mesh/peers/`，启动时清空）。`stations` 优先取**按 BSSID 的终端数之和**：`0x94` 每一路落一个 `peers/<MAC>.<BSSID>.sta`，服务把 30 秒内还活着的那几路加起来（读不到才退回 `<MAC>.metrics` 摘要里的总数——摘要只有最后一帧的数，子节点按射频分别上报时会在两路之间来回换；**一帧不带终端数的状态帧还会把摘要里的 `stations=` 整行抹掉、只剩 `hops=`**，下一次上报又写回来，所以 `.metrics` 不能当"这台报没报过终端数"的证据，要看 `.sta` 文件在不在——2026-11-05 在 `owrt`＋`ax3600` 上八次连读，读到一次只剩 `hops=1`），这时 `metrics_age` 也跟着改成其中最老一路的年龄。这些条目同时以 `bss_detail: [{bssid, stations, age}]` 进 `state.json`（按 BSSID 排序，最多 16 行；总数不受这个上限影响），页面上那一格仍是总数，按 BSSID 的明细是它右边的一列：一路一行 `BSSID 数`，每行悬停是那一路上报的年龄（没有按 BSSID 的条目时那格是 `-`，与"报了 0 个终端"的 `0` 不同）。每 AP 下的终端**列表**（是谁）没有：帧里只有数目。这些数的**问**也在服务这一侧：`ap_metrics_reconcile()` 从拓扑就绪起等 15 秒，之后每 10 秒问一个在线节点（一次调用最多发一条，写 `state.json` 的那一秒循环不受影响），发出 AP Metric Query（`0x800b`，`0x93` 里不列 BSSID＝"你跑的每一路都报"），发出即不读应答——应答按主控自己的 AL MAC 回到 daemon 的 socket，由 trace 库照常落盘，所以原厂子节点（真驱动）与 OpenWrt 子节点（`ap-sta` 那张表）在主控这里是同一条写入路径。节点离开拓扑就不再问，它的 `.sta` 文件自己过期，页面退回"没数据"——这是"这台还没人上报"和"这台不在了"的分别。日志里每次发送一行 `ZTE_MESH_SERVICE ap_metric_query mac=… status=…`（`status=0` 才叫真发出去了）。
- **封禁与踢终端**（`controller-actions.c`）：`ban` 是"持续规则"——首次即发 `0x8016`，之后每 120 秒重发一次（子节点重启后不会忘记），到 `ban.minutes` 或规则被删时补发一条"放行"；`kick` 是"点一下"——`0x8014` 每 5 秒一次、最多 3 次，收到 `0x8015`/`0x8017` 回报即结案，超时标 `failed`。`state.json` 的 `clients[]` 每项 `{mac, action, state, left, attempts}`：`state` 是 `pending`/`sent`/`done`/`failed`/`expired`，`left` 是剩余秒（-1 = 不会到期，`expired` 时也是 -1），计时用服务内部那个单调时钟（`action_table.now`），不是墙钟。定时封禁到期后这一行**留着**并变成 `expired`：删掉的话，下一秒读同一份配置就会把它重新建出来，客户端会被"封禁 / 放行 / 封禁"地循环；改页面上的时长会重新计时并重新封。动作发给**每个在线子节点**，`0x8014` 的目标 BSSID 用广播（还没有"终端挂在哪个 BSS"的表）。
- **调度器和访客限时**：配置里的 `enabled` 是"用户的设置"，有效值 = 设置 减去 定时窗口和已到期的限时；日志 `sched off_all=… off_guest=…`、`access_expired`。`MAX_CONFIG` = 8192 字节。守护进程每次启动都按"重启"计（`ZTE_MESH_START_COUNT`），所以服务单独重启时已入网的子节点会在约 40 到 60 秒内重新入网。守这段的测试（`tests/test_rpcd.sh`、`tools/dev/dev-sched-check.py`，测试环境约 5 分钟真实时钟）条数见 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节。

## 还没在真实路由器上验的

本文不维护那份清单：逐条的验法、前置与判据在 [PLAN](PLAN.md) 第 5 节，能力与"做没做"在 [FEATURE_STATUS](FEATURE_STATUS.md)。下面这些说法**都不等于已验证**——装包后接中兴原厂路由器与角色改成 `master`（H-11；中兴原厂路由器的测试一直用 tmpfs 运行包，程序相同）、封禁/踢终端对端是否照做以及 `0x8014` 用广播 BSSID 能否被接受（H-03，主机侧只验过字节、状态机、配置链）、无线回程端到端（H-01）、额外网络写成多个 BSS（H-02）、终端数与 RSSI 的真值来源（H-05）、软件版本显示到页面与按 BSSID 终端数（H-12、H-13，主机侧做完）、OpenWrt 子节点那一格终端数的整条来源（问、答、采集三段，H-15，主机侧做完）、长时间稳定性（H-09）、多个子节点（H-04）。拓扑页还缺回程 RSSI、树状图与每个 AP 下的终端列表；访客限速在 1905 里没有字段（能力面见 [CONTROLLER_CAPABILITIES](CONTROLLER_CAPABILITIES.md)）。PLAN 之外还有四条没有编号的缺口：LuCI 页面在浏览器里的效果、关主 SSID 的路径（`radioN.enabled=0`、主 SSID 的 `hidden` 热更新、定时 `scope=all`——会断开用户的 Wi-Fi）、子节点开着 V2 而控制器关着的组合、原厂为什么不发 `BeaconMetricsQuery`。

## 复现

构建与跑服务的命令归 [DEVELOPMENT](DEVELOPMENT.md) 第 2、3 节（`sh build.sh`、`tools/build/runtime.sh svctest` 的 tmpfs 运行包、`tools/dev/dev-service-smoke.py --no-attach` 完全私有地跑服务含 `mesh.v2` 重启步骤，接 LAN 要 `--expect-macs`；x86_64 主机用 `Dockerfile.cross` 加 `make ROUTERCC=aarch64-linux-gnu-gcc` 交叉构建）。本文这套映射的测试环境入口是 `python3 tools/dev/device-controller-smoke.py`——真实 `multiapd` 在私有命名空间里初始化，含 CMDU 10。
