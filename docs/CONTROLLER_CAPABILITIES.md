# 原厂控制器能向子路由下发什么配置（全景，含 Z506 的实际支持）

来源：SR1010 的 `multiapd` 与 `libcmapi`/`libmultiapcmapi`/`libieee1905cmapi` 静态分析，对照测试子路由 Z506
（V1.0.0.2B2.8000）自己的 `multiapd`，再加真机抓包。逆向细节见 [FINDINGS.md](FINDINGS.md)，配置语法与验证见
[REAL_CHILD.md](REAL_CHILD.md)。可信度：**[真机]** 抓包或子路由读回验证过；**[代码]** 读到发送/接收路径但没在真机跑；
**[名称]** 只看到函数名或字符串。

> **最重要的一点：两边固件差很多。** SR1010 的 `multiapd`（988 KB，2393 个函数）带大量新功能；Z506 的
> （593 KB，1296 个函数）是旧得多的构建。控制器能发的东西，Z506 不一定能收。下表每项都标了"Z506 有没有对应接收代码"。

## 0. 配置到达子路由的五条通道

| 通道 | 载体 | 性质 |
|---|---|---|
| A. 入网配置 | AP-Autoconfig：搜索/响应/WSC M1-M2/Renew（CMDU 7、8、9、10）+ 紧随其后的 ZTE vendor 消息 | 配 Wi-Fi 本身 |
| B. 策略配置 | Multi-AP Policy Config Request（CMDU `0x8003`） | 配上报/引导策略 |
| C. 信道选择 | Channel Selection Request（CMDU `0x8006`） | 配信道、发射功率 |
| D. 一次性动作 | 各类请求 CMDU | 引导/踢/封终端、扫描 |
| E. 查询 | 各类查询 CMDU | 只读 |

另有一层**控制器本机的管理面**（`CmMultiAP*` → 数据库 → 事件 → 主控模块）：改的是控制器自己的算法参数，
部分会触发通道 B/C/D 往下推。

## A. 入网配置（Wi-Fi 本身）

| 配置项 | 内容 | Z506 | 状态 |
|---|---|---|---|
| WSC M2 | SSID、认证方式（Open/Shared/WPA/WPA2-PSK/混合）、加密（无/WEP/TKIP/AES）、密钥；每频段、每 BSS 槽位 | 有 | **[真机]** 主 SSID 与密钥；访客等槽位的名称、密码、开关 |
| Multi-AP 扩展子元素 | 每 BSS 的类型：Tear Down（关闭）、Fronthaul、Backhaul | 有 | **[真机]** |
| Renew | 让已入网的子路由重新跑自动配置 | 有 | **[真机]** 改配置后约 4 秒重启无线并应用 |
| Backhaul BSS Config TLV | 回程 BSS 的凭据 | 无接收代码 | [代码] |
| ZTE vendor：频段配置（FreqBand） | 只有三个子 TLV：`0x07` BeaconInterval（每频段，毫秒，4 字节大端）、`0x19` MaxHop、`0x55` STA 绑定支持标志；**没有 Fragment、TxPower、带宽、信道**。Z506 只处理 `0x07`、`0x19`（`0x55` 报 `unknown sub type`） | 有（`0x07`、`0x19`） | **[真机]** 2.4G 改 120 后 `WLANBase` 读回 120，5G 不变；改回读回 100 |
| ZTE vendor：SSID 配置 | 按槽位序号：SSID 使能、ACL 策略。其余子类型（访客标志、VAP 隔离、隐藏 SSID 等）报 `unknown sub type` 并忽略 | 部分 | **[真机]** 只应用"使能 + ACL 策略" |
| ZTE vendor：ACL 列表 | 每频段完整 MAC 名单（名称/MAC/策略），子路由删除多余、新增缺的 | 有 | **[真机]** 已下发（`acl.N.*`），`AclCfg` 表与驱动读回，一加被拒；`allow`（数值 2）来自子路由代码，未在真机试 |
| ZTE vendor：STA 绑定 | 把某终端绑定到某频段/AP（`CmMultiApSetStaBind`） | **无** | [代码]（仅 SR 侧） |
| 哪些 vendor 消息启用 | `P1905AcdVenderCfg`（`CmMultiAPSetAcdVendorCfg`）决定启用清单 | — | [名称] |
| M2 里的 ACD vendor TLV、默认 802.1Q、流量分离、Profile-2 能力 | VLAN / 多 profile 相关 | **无** | [代码]（仅 SR 侧） |

## B. 策略配置（CMDU `0x8003`）

由 `PolicyConfigRequestSend` 组装，下列 TLV 都可能出现：

| TLV | 内容 | Z506 | 状态 |
|---|---|---|---|
| Metric Reporting Policy（`0x8a`） | AP 指标上报间隔；每无线电的 STA RSSI 上报阈值、滞回、信道利用率阈值、流量统计/链路指标标志 | 有 | **[真机]** 子路由 `rssithr` 随之变化（0 与 -110） |
| Steering Policy（`0x89`） | 禁止引导的终端名单；每无线电的引导策略、利用率阈值、RCPI 引导阈值 | 有 | [代码] 抓包里没出现（控制器没有该无线电信息时不发） |
| Unsuccessful Association Policy、Channel Scan Reporting Policy、Traffic Separation / 默认 802.1Q | 关联失败上报、扫描上报、VLAN 隔离 | **无** | [代码] |
| ZTE vendor 策略 TLV | 见 B.1 | **无解析代码** | **[真机]** 已发送，子路由丢弃 |

### B.1 ZTE vendor 策略子 TLV（只有 SR 侧有 Insert，**Z506 没有 Extract**）

漫游阈值（各频段 RSSI 与负载限、定时器）、802.11r 使能、BTM 踢人、VLAN 配置、兼容版本、Roam Caton 参数、STA 掉线信息、
`P1905ComCfg` 同步、VBSS。**更正**：早先说"漫游阈值已送达并被子路由确认"，其中"确认"只是 CMDU 层 ACK；Z506 固件里没有
解析代码，所以漫游阈值**不会**在这台子路由上生效（它的 `MultiAPDomain.RoamRssiLmt` 一直没变，与此一致）。

## C. 信道选择（CMDU `0x8006`）

| 内容 | Z506 | 状态 |
|---|---|---|
| 每无线电的信道偏好、发射功率上限 | 有 | [代码] 子路由 `slave_set_radio_cfg` 校验后写入**信道、发射功率、无线电使能**并复位射频 |
| 带宽（20/40/80/160） | 控制器侧 `master_set_radio_info` 会按"信道 + 带宽"算运行类别和中心信道，并拒绝正在用于回程的信道；**但 Z506 不写 `BandWidth`**，也不改"自动信道"开关 | [代码] |

## D. 一次性动作

| 动作 | 控制器接口 | Z506 | 状态 |
|---|---|---|---|
| 把终端引导到另一个 AP/频段（BTM） | Client Steering Request；`CmMultiApSetStaBTMRequest` | 有 | [代码] |
| 回程引导 | Backhaul Steering Request | 有 | [代码] |
| 封锁/放行终端 | Client Association Control Request；`CmMultiAPAddBsFilter` 等 | 有 | [代码] |
| 踢终端 | `CmMultiApSetStaDisassociate` | 经 BTM/断开 | [名称] |
| 信道扫描、CAC、VBSS、无线电命令 | `CmMultiAPChannelScan`、`CmMultiAPSendCAC`、VBSS 系列、`VendorRadioCommand` | **无** | [代码]（仅 SR 侧） |
| 终端/邻居/未关联终端/Beacon 指标测量 | 各 Query CMDU | 有 | [代码]；AP 指标查询 **[真机]** 周期性有应答 |
| 高层数据透传 | Ctrl/Agent Higher Layer Data | 有 | [代码] |

## E. 查询（只读）

AP 能力、信道偏好、客户端能力、AP 指标、关联终端链路指标、拓扑、链路指标、设备信息/清单：Z506 都有接收代码。
**[真机]** 拓扑、链路指标、AP 指标有应答；AP 能力、信道偏好、客户端能力查询在入网初期被子路由丢弃（它还不认识控制器），
之后控制器不再重发。

## F. 控制器本机的管理面（CM 接口 → 数据库 → 事件）

原厂界面/脚本用的"配置入口"，主要改控制器自己的行为，一部分会触发上面的通道。

| 分组 | 接口 | 事件/去向 |
|---|---|---|
| 角色与总开关 | `CmMultiAPSetMultiAPCfg`（Mode、Enable、SyncEnable…）、`CmIeee1905AcdSynEnableSet`、`CmMultiApTopoSetEnable` | 本机配置 |
| 主控策略 | `CmMultiAPSetMasterCfg`（`EnRoam`、`EnBandSteer`、上报阈值与间隔…） | 同步消息 `SSEND(0x2401, 0x10205)` → `multiapd.map.master`（不是事件号）；改上报策略会重发 `0x8003`；shim 已实现（`zte-meshctl notify-master`） |
| 域（按 SSID 匹配 BSS） | `CmMultiAPSetDomainCfg`、`SetRoamCfg` | 同步消息 `SSEND(0x2401, 0x10206)` → `master_update_roam_threshold`；shim 已实现 |
| 频段引导 | `SetBSCfg`、`SetBSEnable`、`SetBsPolicies`、`Add/DelBsFilter`、`SetBsFilterList` | 控制器判断逻辑 |
| 漫游过滤/缓存 | `AddRoamFilter`、`DelRoamFilter`、`AddCacheMac`、`DelCacheMac` | 控制器判断逻辑 |
| BTM 与 Caton | `SetBtmReqModeCfg`（`0x10208`）、`SetRoamCatonCfg` | 控制器判断逻辑；Caton 会触发 vendor 策略 |
| 回程 BSS | `SetBHBssEnable` | 影响 M2 里的回程 BSS 标记 |
| 代理频段引导 | `CmMultiAPSetAgentBsCfg`（`0x10238`） | 本机 slave 模块 |
| 无线电信息 | `CmMultiApSetRadioInfo`（`0x10216`）、`master_set_radio_cfg`（`0x10203`） | 通道 C |
| 终端动作 | `SetStaBTMRequest`（`0x10226`）、`SetStaDisassociate`（`0x10227`） | 通道 D |
| 同步类 | `CmMultiApTopoSetP1905ComCfg`、`CmMultiApSetStaBind`、`CmWLANMapSsidCfgSync2Repeater` | vendor 策略 / 入网消息 |
| 维护 | `CmMultiAPWlanRenew(Notify)`、`CmMapRestartWithDBReset`、`CmMultiApTopoDelOfflineDev` | 通道 A、重启 |
| WLAN 级（libcmapi） | `CmWLANSetDualBandSync(Only)`、`CmWLANSetBandSteer`、`CmWLANSetMlo(En)`、`CmSetRoaming` | 本机 WLAN 配置（子路由 Web 上那些开关） |

## G. 对本项目的含义：Z506 上控制器真正能管的

| 能管 | 说明 | 本项目 |
|---|---|---|
| 主 SSID、密码、开关；多 BSS（访客等）的名称、密码、开关 | M2 | 已实现，**[真机]** |
| 每无线电的 BeaconInterval（20 到 1000 ms） | vendor FreqBand | 已实现（`radioN.beacon_interval`），可热更新，**[真机]** |
| MaxHop | vendor FreqBand | 固定下发 2（与子路由原值一致），无配置项 |
| 控制器的 `CurHop` | Higher Layer Response 的厂商 TLV（子类型 `0x43`） | 启动时写 2；**[真机]** 子路由读到 2（原先 -2，见 FINDINGS 第 7 节） |
| 自组网 V2（控制器自己的 `P1905ComCfg.Enable`） | 本机准入检查，不下发 | `mesh.v2=0\|1`，**默认 0**；**[真机]** 两种取值都正常；改动 = 重启守护进程（子节点掉出拓扑 8 到 58 秒再入网）；`PolicyConfigReq` 的 `60[2]` 首字节跟着变（`0101`/`0001`），子节点没采用 |
| 控制器自己的角色（主路由 / 自适应） | `MultiAPCfg.Mode`/`SetRoleEnable`，不下发 | `mesh.role=master\|auto`：auto 热更新 = 守护进程重启两次；**[真机]**；Slave 侧不支持 |
| 子路由的角色（自适应→Slave） | 子路由自己的选举，控制器只需响应 | **[真机]** 选举能被接受；触发由子路由 `cspd` 负责（FINDINGS 第 3 节） |
| Fragment 门限 | **没有任何下发通道** | — |
| 每 BSS 的 ACL 策略（0 关闭、1 拦截、2 仅放行）与名单 | vendor SSID/ACL | 已实现（`acl.N.mac/name/radio/bss`、`radioN.acl_policy`），**[真机]** 表与驱动读回、一加被踢并拒绝重连；`allow` 与 5G 读回未验证 |
| 终端上报阈值（Metric Reporting Policy） | `0x8003` | 已按子路由原值 -110，**[真机]** 读回 |
| 引导策略（Steering Policy） | `0x8003` | 未提供配置项 |
| 信道、发射功率 | `0x8006` | 已实现（`radioN.channel`、`tx_power`），**[真机]** 两频段验证并还原 |
| 引导/封锁/踢终端 | `0x8014` 等 | 未提供 |
| 重启子节点、指示灯开关 | 隧道 VendorSpecific（内层 TLV `0x20`/`0x21`） | 已实现（服务节点动作、rpcd、LuCI 按钮），**[真机]** |
| 访客限时、Wi-Fi 定时关闭 | 控制器本机定时改槽位开关 + Renew | 已实现（`access_minutes`、`timer.*`），**[真机]** 访客范围；`scope=all` 仅实验室 |
| Wi-Fi 5 兼容（`ForceWiFi5`） | 槽位应答 `0x46` | 已实现（`force_wifi5`），线上值正确；Z506 不认 |
| 访客限速 | **没有下发通道**（子节点本机功能） | — |

**Z506 管不了的**（固件没有对应代码，不是我们没做）：漫游阈值（vendor 策略 TLV）、802.11r/BTM 踢人、VLAN 与流量分离、
STA 绑定、信道扫描/CAC、VBSS、MLO，以及**带宽**。要用这些得换较新固件的子路由。

## H. 互通控制与协议代际：`P1905ComCfg`、`VersionMode`、`CurHop`

这几个字段不下发"配置"，但决定两台设备能不能组网、用哪一代行为（详见 [FINDINGS.md](FINDINGS.md) 第 5 到 7 节）：

| 字段 | 作用 | Z506 | SR | 本项目 |
|---|---|---|---|---|
| `P1905ComCfg.Enable`（界面"自组网 V2"） | 接收方向的准入检查：关 = 谁都接受；开 = 对端必须带 ZTE 厂商 TLV、厂商一致、市场码一致 | 有 | 有 | `mesh.v2`，默认 0 |
| `MarketConfEn`/`MarketSign` | 市场码检查与市场码 | 有 | 有 | 真机路径设 1 / `8000`（`ZTE_MESH_REAL_MARKET_SIGN`） |
| `EnableIsSync` | 主路由把 `Enable`、`MarketConfEn` 同步给子节点 | 无解析代码 | 有 | 不启用（0） |
| `Policy`/`DropNum`/`DropRoam` | 对未通过对端：0 只记录、1 拒绝、2 限制（通知无线管理并随机丢 Client Steering Request） | 无 | 有 | 全 0 |
| `A1Enable` | WPS M1 型号名带 "A1" 的产品线门槛 | 无 | 有 | 0 |
| `MultiAPCfg.VersionMode` | 功能位掩码：bit1 = 标准行为/不发私有厂商 TLV，bit3 = Profile-2，bit7 = Wi-Fi 7 MLD，bit0 = 插件 BSS/转发；子节点跟随控制器 | 默认 0 | 产品相关 | `version_mode=128`（bit1 清零 = 仍发私有 TLV，bit7 顺带开 Wi-Fi 7 TLV） |
| `CurHop` / `MaxHop` | 控制器告诉子节点的跳数预算 | 解析并采用 | 有 | `CurHop=2`、`MaxHop=2` |
| `Mode`/`SetRoleEnable` | 角色（自适应/Slave/Master）与自适应开关 | 有 | 有 | 控制器固定 `Mode=2`、`SetRoleEnable=1`、`EnMasterOnly=1` |

## 方法与局限

函数集合取自 `radare2`（`afl`），按名字比对两边；同名不一定同行为，差异清单只说明"有没有对应代码"。事件编号表取自
`multiapd` 数据段（`0xe1520` 一带的"事件号 → 处理函数"数组）。各 vendor TLV 内部字段只还原了漫游阈值和（部分）
Metric Reporting Policy，其余只确认存在。抓包只覆盖本项目实际触发过的流程。
