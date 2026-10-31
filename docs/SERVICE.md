# B.4：长期运行的 Controller 服务模式、节点清单与配置下发

> **状态（2026-10-18）：** 服务模式已与真实子节点 Z506 联调（入网、配置下发、热更新、守护进程重启、节点动作、调度器，见各节与
> [REAL_CHILD.md](REAL_CHILD.md)），并打成 OpenWrt 包在真实 procd/rpcd 上跑过（隔离网桥）。

## 已成立的事实

- **节点清单**：`P1905DeviceInfo`（11 字段/788 字节/1024 行，主键 ViewName+MacAddr）从 `cspd` 静态恢复；真实 `multiapd`
  收到对端后自己写入行（AL MAC、Active、WSC M1 带来的 ModelName）。`controller_read_nodes()` 只读导出，不含序列号/ProductClass。
- **服务模式** `ns-run … --controller-service`（`controller-service.c`）：没有限时，配置由宿主写 `/run/zte-mesh/config`
  （0600，严格 key=value），状态每秒写 `/run/zte-mesh/state.json`（`/run/zte-mesh` 是沙箱里唯一可写的宿主路径）。
- **配置下发链路**：改 profile → 事件 `0xa93e` 发给 `multiapd.acd.acd` → 原厂 `_handleWlanRenew` → 真 daemon 发
  AP-autoconfiguration Renew（CMDU 10）。`CmMultiAPWlanRenew` 本来发给 `cspd.cspd.multiapd_mgr`（事件 `0x9c8a`），
  我们不跑 cspd，所以 helper 直接发 `0xa93e`（8 字节 `{band,0…}`）。
- **外部进程不能调 CM**：`ASEND/SSEND` 需要 OSS 身份，且要用客户端库 `liboss_pub.so` + `InitAppComm`（工厂 `diagget` 的做法），
  不能用 daemon 的 `liboss.so`；从监督进程直接调会崩。所以 `zte-meshctl`（ARM32）链 `liboss_pub`，由服务 fork/exec。
- M2 的 Authenticator/Key Wrap/profile 由独立校验器通过（`wsc_cryptographic_verified` 进报告）。
- 标准模式（`VersionMode` bit1）下节点 `ZTEDevFlag=0`，型号来自 WSC M1。

## MLO 开关与跨设备模块消息（`0200fe`）

- 配置键 `mlo.enable=0|1`（默认 1，与原厂 `WLANSetting.MLOEnable=1` 一致），启动时写库，可热更新。`wifi.dual_band_sync`
  同时写 `WLANSetting.DualBandSync`（只是启动值）。
- 原厂 `cspd.wlan_manage.wlan_mgr` 在设备上线（及 MLO 开关变化）时，经 `CfIeee1905TunnelToDevice` **异步**发事件 `0xa97e`
  给 `multiapd.topo.topo`，daemon 把它作为厂商 TLV `0200fe` 发给子节点：体 =
  `{u16 BE 36, 名字[36]="cspd.wlan_manage.wlan_mgr", u32 BE 1, u32 BE 4, u32 BE 值=MLOEnable}`。
  服务在 ZTE 子节点在线满 15 秒后调 `zte-meshctl wlan-mgr <AL MAC> <0|1>`（每个子节点各自计时，最多 8 个，每个最多重试 4 次）。
  **必须用 `ASEND`**：同步 `SSEND` 走请求表，不含 `0xa97e`，返回 0 但线上没有帧。
- 原厂入网时发 3 条，shim 每个会话发 1 条。Z506 的 `WLANSetting` 没有 `MLOEnable` 字段，所以对它是无操作。
- 启动时还要注册三张空视图/小表（`MultiAPRoamCatonCfg`、`WLANSetting`、`MultiAPCacheMac`），否则第一条
  `PolicyConfigReq` 的厂商体会塌成 1 字节。

## CM 通知层：策略类设置的热更新

原厂 CM 设置函数（`libmultiapcmapi.so`）写库后 `CmSendMsg2MM("multiapd.map.master", 0x2401, 消息号, name[32], 无负载)`，
即**同步** `SSEND(0x2401, {u32 消息号, char name[32], u32 0})`，主控在 `master_handle_synmsg` 里按 SynSet 表分发。
直接写库（`sendcmd 1 DB set`）不会发它，`multiapd` 既不重读也不推送。

| 消息号 | 谁发 | 处理 | 线上效果 |
|---|---|---|---|
| `0x10206` | `SetRoamCfg`、`SetDomainCfg`、`SetBSCfg`、`SetBSEnable` | `_master_set_doamin_cfg` | 每个子节点一个 `PolicyConfigReq`：`MetricRptPolicy` + 厂商 `4a[12] 4d[1] 60[2]`（83 字节） |
| `0x10205` | `SetMasterCfg`、`SetMultiAPCfg` | `_master_set_master_cfg` | 纯厂商 TLV 的 `PolicyConfigReq`：`4c[2] 5a[1] 4d[1] 60[2]`（52 字节） |
| `0x10208` | `SetBtmReqModeCfg` | `_master_set_btm_req_mode` | 无帧（只更新判断参数） |

服务里的实现：`zte-meshctl notify-master <id>...`（只允许 `0x10205`/`0x10206`/`0x10208`，可多个、同进程按序发；
原厂网页路径两条通知间隔 12 ms）；`mesh-policies.c` 的 `mesh_policies_live_*()` 在池锁下写全部 8 个 `MULTIAPDOMAIN` 行或 Master 行 0。
Domain 字段变 → `0x10206`，Master 字段变 → `0x10205`，两者都变按原厂顺序先 06 再 05；启动时不发。

热更新范围：已有槽位的 ssid/key/hidden/beacon_type、主 SSID 的 `hidden`/`beacon_type`、`mlo.enable`、`beacon_interval`、
信道/功率、ACL、`wifi.dual_band_sync`、`roam.*`、`band_steer.*`、`roam.force`、槽位 `enabled`。WLAN 类变化**只 Renew 变化了的频段**。
键的出现/消失（无→有、有→无）要重启服务（`policy_keys_changed`）。

## 守护进程重启：mesh 级变化

原厂的"mesh 级 Renew"其实是 cspd 的 `multiapd_mgr` 重启 `multiapd`（`CmMultiApTopoSetP1905ComCfg` 写库后无条件
`ASEND(0xa93d)`；`CmMultiAPSetMultiAPCfg` 在 `Mode`/`Enable`/`VersionMode` 变化时 `ASEND(0xa93c)`）；两个频段的 Renew 和
6 到 40 秒的拓扑抖动是新进程起来的副作用。

服务里（只做了 `mesh.v2`）：`mesh.v2` 改动 → `v2_update`（池锁下写 `P1905ComCfg.Enable` 并读回）→ `daemon_respawn`：
SIGKILL 旧进程、`waitpid`、探测数据库池锁与订阅池锁（被杀的进程可能握着锁，失败则服务退出交给 init）、在同一数据库上 fork/exec。
数据库和订阅池属于监督进程，不丢；`state.json` 的 `daemon_pid` 换新、`daemon_restarts` 加 1。写库或读回失败退回 `restart_required`。

- 重启后清掉推送记录，`0200fe`、信道/功率要等 `P1905DevNum >= 2` 再计 15 秒：新进程起来时库里 `Active` 是旧进程留下的 1，
  抖动期推出去的消息会被丢弃。
- 原厂重启后第一对 Renew 由 ACD 的 3 秒定时器触发：`acdsCheckMultiapdRestart` 向 `cspd` 取启动次数（`SSEND(0xa96e)`）。
  shim 里没有 cspd，`controller-route.c` 拦截那一条，按 `ZTE_MESH_START_COUNT` 回答（首次开机为 1，冷启动行为不变）。
  **不要自己发 ACD 事件 2**：守护进程收到第一个 1905 帧时会自己发，提前发会浪费一次性的 `g_AcdsRenewStat`。
- 真机（15 次重启）：子节点掉出拓扑、`TopoResponse` 不带 `1905Nbr`、8 到 58 秒后重新入网；`PolicyConfigReq` 的 `60[2]`
  跟着 `mesh.v2` 变。新进程要收到子节点的下一帧才往外发，所以重新入网跟着子节点的周期帧走。
- 仍要重启服务的：`version_mode`、增减槽位、槽位 `enabled`。

## 设备角色与 CurHop

原厂"自适应" = `Mode 0` + `SetRoleEnable 1` → 守护进程重启；cspd 的 `cooperator_mgr` 判定路由模式 → ACD 的
`_autoSetRole(2)` 写 `Mode 2`、`SetRoleEnable 1`，再要求第二次重启（约 3 秒后）。

- 可选键 `mesh.role=master|auto|slave`（UCI 侧的新取值见"设备角色"）：`master`/`auto` 写 `Mode 2` 和 `SetRoleEnable`（0/1）；
  `slave` 写 `Mode 1 / SetRoleEnable 0 / EnMasterOnly 0 / CurHop -2`，并在运行时把主控下发的 Wi-Fi 配置发布成
  `/run/zte-mesh/wlan-plan`（`agent-sync.c`），由沙箱外的 `wlan-sync` 落到 `/etc/config/wireless`。
- plan 有两个生产者，按角色互斥：子路由由服务写；`main`（主路由）由 init 的 `write_config` 直接写（`write_local_plan`，格式同）；
  仅主控不写，还会删掉旧文件。`wlan-sync` 不关心是谁写的。
- 热更新 `master→auto`：写 `SetRoleEnable 1`、`Mode 0`，重启守护进程；约 3 秒后写 `Mode 2` 再重启一次——**第二步是监督进程
  替 cspd 的 `cooperator_mgr` 做的模拟**（守护进程自己不会把 Mode 写回 2）。`auto→master` 只清 `SetRoleEnable`，不重启。
- `CurHop` 恒为 2：`EnMasterOnly=1` 让原厂会把它重置成 -2 的 `slave_init` 跳过；线上 `HigherLayerResponse` 子 TLV `0x43`
  与原厂一致（原厂的 -2 只是库里几秒的瞬态）。
- 没做：Slave 侧、桥接模式判断、ACD 自己的 1905 选举协议。

## UCI 与 LuCI 映射

`zte-mesh.init` 的 `write_config` 把 `/etc/config/zte_mesh` 翻成服务配置（`/run/zte-mesh/config`，0600，原子替换）；
`luci-app-zte-mesh` 的 `zte-mesh.js` 是对应表单。init 只为让日志说出哪个选项不对而检查，服务自己仍会再校验整份文件；
任何一项不合法都不启动/不替换配置（日志 `zte-mesh: <选项> ...; not starting`，不记录 SSID 和密钥的值）。

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
| `mesh_v2`、`mesh_role`、`mlo_enable` | `mesh.v2`、`mesh.role`、`mlo.enable` | `mesh_v2` 即原厂 MESH 开关（`P1905ComCfg.Enable`）：开 = 只接受市场码相同的中兴子路由；关 = 接受任何子路由（默认） |
| `band_steer*`、`roam*` | `band_steer.*`、`roam.*` | 两个频段的阈值必须成对给出 |

### 设备角色（三个）

原厂那台"主路由"既当主控又自己发 Wi-Fi；SR1010 这类刷 OpenWrt 的机器大多是有线网关、没有无线，所以把一个开关拆成三个角色：

| `mesh_role` | 下发档案 | 应用到本机 `/etc/config/wireless` | 写 `mesh.role` |
|---|---|---|---|
| `master`（默认，未设置也等于它） | 是 | **不动** | `master` |
| `main` | 是 | **是**（plan 由 `write_config` 写） | `auto` |
| `slave` | 否 | 是（plan 由服务从 M2 读出来写） | `slave` |

`main` 就是原厂的"主路由 / 自适应"，线上仍发 `auto`（daemon 一侧一行没改，`Mode 0 → 2` 的选举替身照旧跑）；旧值 `auto`
继续被接受。仅主控会删掉 `wlan-plan`、**不启动** `wlan-sync` 实例，射频存在时日志会明说"本机无线不动"。

包的默认配置（2026-10-19 起）把能取出厂值的都写上，LuCI 显示同样默认值并标注范围：`mesh_v2=0`、`mlo_enable=1`、
`wifi_dual_band_sync=1`、`wifi_5g_suffix=-5G`、`wifi_24g_ssid=ImmortalWrt`、安全方式 2.4G `sae-mixed` / 5G `psk-mixed`、
信标 100 ms、信道 0、功率 20/23 dBm（子节点最大档 = 出厂值）、`band_steer=1`、引导阈值 -60/-110、漫游阈值 -60/-70、
`roam_force=0`、`enabled=1`、`wifi_24g_key=Ztemesh2631!`（清空则服务不启动）。要让子路由保持自己的信道/功率，删掉对应选项。

其余区段：`zte_mesh_bss`（额外槽位，`band`/`slot`/`enabled`/`ssid`/`key` 必填，同频段槽位必须 1..k）、
`zte_mesh_timer`（最多 8 条，`start`/`end` 必填，`days`、`scope` 可选；init 把 `date +%z` 换算成 `timer.utc_offset`，
**夏令时切换后要重新保存设置**）、`zte_mesh_acl`（最多 16 条，按文件顺序编号）、`zte_mesh_node`（别名，只给 LuCI 节点表）。

重载：`reload_service` 只在"需要重启服务"的东西变了才重启——`version_mode` 变了，或策略键（`mesh.role`、`wifi.dual_band_sync`、
`band_steer.*`、`roam.*`、`mlo.enable`）出现/消失（init 把键名存进 `/run/zte-mesh/restart.sig` 比较）；值变了不重启，热更新。

测试（都不需要 OpenWrt）：`tests/test_init.sh`（busybox ash + UCI 桩，79 条）、`node tests/test_luci.js`（表单跑一遍，
核对每个 init 读的选项都有表单项）、`dev-config-check.py DIR`（把 init 生成的配置交给真服务，报告接受/拒绝）。

对已有配置的影响：随包模板新增 `option wifi_dual_band_sync '1'`（conffile，已有配置不改）——**没有这一行、又留空
`wifi_5g_ssid` 的旧配置以前拒绝启动，现在会派生 `<2.4G 名称>-5G` 并启动，也就是会改子节点真实的 5G 名称。**
另外 `radio1.ssid/key` 变成可选；init 归一化布尔值并拒绝不可打印字符；带槽位的 ACL 规则以前被整份拒绝，现在接受
（旧 bug：检查在槽位表填好之前执行），但**没有在真子节点上试过槽位上的 ACL**。

## 节点管理、拓扑摘要、定时与访客限时

- **节点动作**：宿主往 `/run/zte-mesh/actions/<id>.act`（0600，文件名只用 `[A-Za-z0-9_-]`）写一行 `reboot <mac>` 或
  `light <mac> 0|1`；服务每秒取走（读完即删，启动时先清遗留），只对在线且已识别为中兴设备的节点执行（`topology_ready` 之后）。
  结果进 `state.json` 的 `actions`（最新在前，最多 8 条；`status` 0 已发出、-2 格式错、-3 无此节点、-4 守护进程没就绪）。
  rpcd `luci.zte-mesh reboot_node {mac}` / `light_node {mac,on}` 写请求并最多等 8 秒（-5 = 服务没运行）。
- `state.json` 的节点多了 `light`（-1 未知）、`light_age`、`stations`（-1 未知）；`sched` 有 `rules`、`off_all`、`off_guest`、
  `access`（各访客倒计时）。`light`/`stations` 来自 trace 库对子节点帧的摘要（`/run/zte-mesh/peers/`，启动时清空）。
- **调度器和访客限时**：配置里的 `enabled` 是"用户的设置"，有效值 = 设置 减去 定时窗口和已到期的限时；日志
  `sched off_all=… off_guest=…`、`access_expired`。
- 守护进程每次启动都按"重启"计（`ZTE_MESH_START_COUNT`），所以服务单独重启时已入网的子节点会在约 40 到 60 秒内重新入网。
- `MAX_CONFIG` = 8192 字节。测试：`tests/test_rpcd.sh`（15 条）、`dev-sched-check.py`（实验室，约 5 分钟真实时钟）。

## 未验证 / 已知缺口

- 用**已安装的包**接真实子节点（真子节点测试一直用 tmpfs 运行包，程序相同）；LuCI 页面在浏览器里的效果。
- 关主 SSID 的路径（`radioN.enabled=0`、主 SSID 的 `hidden` 热更新、定时 `scope=all`）——会断开用户的 Wi-Fi。
- 拓扑页：回程 RSSI、树状图、每个 AP 下的终端列表、软件版本字段；终端数没有独立核对。
- 访客限速（1905 里没有字段）、长时间稳定性、多个子节点、子节点开着 V2 而控制器关的组合。
- 原厂为什么不发 `BeaconMetricsQuery`。

## 复现

```sh
sh build.sh                                        # mac：完整构建 + 主机测试
python3 device-controller-smoke.py                 # 实验室：真实 multiapd 在私有命名空间里初始化（含 CMDU 10）
sh tools/build/runtime.sh svctest                  # tmpfs 运行包（启动器根 /tmp/zte-mesh-svc/root）
python3 dev-service-smoke.py --no-attach           # 完全私有地跑服务（含 mesh.v2 重启步骤）；接 LAN 需 --expect-macs
```

x86_64 主机可用 `Dockerfile.cross`（`make ROUTERCC=aarch64-linux-gnu-gcc`）交叉构建。
