# 逆向与真机发现汇总

已经查实的事实，按主题组织。操作流程见 [DEVELOPMENT.md](DEVELOPMENT.md)，配置语法与验证结果见
[REAL_CHILD.md](REAL_CHILD.md)，控制器能下发什么见 [CONTROLLER_CAPABILITIES.md](CONTROLLER_CAPABILITIES.md)。

可信度标记：**【真机】** 在测试子路由 ZXHN Z506（V1.0.0.2B2.8000）上抓包或读回验证过；**【代码】** 读到反汇编，没在真机跑；
**【推测】** 由证据推断。没有标记的是定义或来源说明。

## 1. 总体原理

不重写 mesh 控制器：把原厂 ZTE 路由器上的 ARM32 `multiapd` 搬到 SR1010（ImmortalWrt，无 Wi-Fi，只做有线网关）上当控制器，
周围用一层"垫片"伪造它依赖的环境，再用一个配置文件间接指挥它。

```
/run/zte-mesh/config ──▶ controller-service ──▶ 私有 netns 里的原厂 multiapd ──IEEE 1905 (0x893a)──▶ 子路由 multiapd ──▶ cspd ──▶ 无线驱动
                              │  写数据库行 / 触发 Renew / 自己发 0x8006        ▲
                              └───────────────▶ 原厂共享内存数据库（表结构从二进制还原）◀── CM 接口替身 libzte_controller_profile.so
```

- **数据库**：原厂把配置放在共享内存数据库（`libdb`）里，表结构（字段、类型、行数）从 `cspd` 的建表函数反汇编还原
  （`startup-schema.py`、`schema.py`、`profile_schema.py`）；服务启动时把 SSID、密码、BSS、BeaconInterval、ACL 等写成行。
- **CM 替身**（`controller-profile.c`，190 行）只替换两个函数：`CmWLANGetAPConfig`、`CmWLANGetPsk`。原厂版本会走内部 WEP setter、
  给 `cspd` 发消息、查无线驱动，SR1010 都没有。替身从我们填的 `WLANCfg`/`WLANPSK` 行读，按 43 个偏移搬进 2496 字节的 CM 输出，
  只接受 WPA2-PSK + AES，要求 `ZTE_MESH_DB_SANDBOX=1`。其余 CM 调用走的都是原厂真库。
- **不启动 `cspd`**：守护进程需要的"路由器其余部分"由数据库行 + 替身 + 身份环境变量提供。
- 服务自己构造的唯一一类 1905 报文是信道选择请求（CMDU `0x8006`），其余都是原厂守护进程自己发的。

## 2. 两份固件不是同一个构建

| | 子路由 Z506 | SR1010（我们跑的控制器） |
|---|---|---|
| `multiapd` | 593 KB，1296 个函数 | 988 KB，2393 个函数 |
| ZTE vendor 策略 TLV 解析（漫游阈值、11r、BTM 踢人、VLAN、`P1905ComCfg` 同步、VBSS） | **没有** | 有 |
| STA 绑定、CAC、信道扫描、流量分离、Wi-Fi 7/MLO | 没有 | 有 |
| `P1905ComCfg` 表 | 3 个字段（`Enable`、`MarketConfEn`、`MarketSign`） | 8 个字段（见第 5 节） |

两份 `libcmapi.so` 也不同（SHA256 不同）。M2 由 SR1010 一侧构造，所以控制器行为读 SR1010 的二进制，
子路由能不能收读子路由自己的。 【真机/代码】

## 3. 子路由的角色、选举与"组网"

### 3.1 字段
- `MultiAPCfg.Mode`：0 自适应、1 Slave、2 Master、3 两者。`Enable`：MESH 总开关。
- `SetRoleEnable`：自适应开关；网页选"自适应"时置 1，选其它模式置 0（`internet_mesh_common_lua.lua`）。
- `CurHop`、`MaxHop` 见第 7 节；`P1905_ACD.ACDRole` 是 ACD 的 Server/Client 角色。

### 3.2 自动角色选举（`source/acd/roleplay.c`）【代码】
- `SetRoleEnable != 0` 且 `Mode == 0` 时 ACD 角色"未定"，不会开始自动配置。
- 选举：对每个接口发 AP-Autoconfig Search（`_autoSendSearch`），收到控制器的响应（支持频段、Supported Service 为控制器、
  消息号匹配）就 `_autoSetRole(1)`；超时没有邻居就当 Controller；多台自适应设备按 MAC 比较。
- `_autoSetRole(角色)` 写 `Mode`/`SetRoleEnable=1`，再通知 `cspd`（`ASEND` `0xa26d` → `cspd.cspd.multiapd_mgr`）。
- 触发事件：`0x909b` 开始、`0x909e` 停止、`0x9095` 直接设为控制器。调试命令 `sendcmd multiapd.base.base roleplay`。

### 3.3 谁触发选举：`cspd` 里的两个调用点【代码】+ 真机读内存【真机】
`CfIeee1905RoleElect` 在 `cspd` 里只有两处调用：`MapCooperatorACDUpProc`、`MapCooperatorV4AddrProc`。全局标志
`g_TriggerSelectFlag`（`.bss` 0x203370）两位：bit0 = br0 已获得 IPv4 地址；bit1 = 收到 ACD 上线通知且当时 `Mode == 0`
（且 `IsBridgeMode() == 1`）。**两位都为 1（值 3）时，后到的那个事件才调用 `CfIeee1905RoleElect`。** 本机实测
`IsBridgeMode()==1`（`WANCPPP.WANCName="Bridge"`）、`AutoSetup=0`，读 `cspd` 内存得 `g_TriggerSelectFlag=1`（缺 bit1）。

### 3.4 观察到的行为
- 手动设 Slave：选举没有发生，子路由照常入网。
- 改成自适应后（网页改不会重启 `multiapd`）：**子路由一个 Search 都不发**，停在"角色未定"，所以"组网失败"。【真机】
- `sendcmd multiapd.base.base roleplay` 手动开始选举：子路由发 Search、控制器回 Response、M1/M2 完成，`Mode` 0→1。
  说明控制器能被选举接受，缺的只是触发。【真机】
- 用户设好三项并**重启**子路由：自行选举为 Slave，`CurHop=2`、`P1905DevNum=2`。【真机】
- 反复 `kill` 子路由 `multiapd` 后，每次启动都有 `SubScribPublish server 10022 already exist!`、
  `acdInit SubScribPublish multiapd ACD Service Fail`、`ACD Notify event[4] to 0 clients`，计数
  `multiapd restart unexpected 17 times`——通知发给了 0 个订阅者，bit1 永远置不上。**所以不要再用 `kill multiapd`
  触发重新入网。** 【真机】

## 4. 入网准入链

控制器是原厂 `multiapd`，子路由的接纳逻辑是一串门槛，每项都经反汇编确认并在真机上从失败走到成功（表见
[REAL_CHILD.md](REAL_CHILD.md) 第 2 节）：设备身份（ZTE / `ZXSLC SR1010`）、市场码（`MarketConfEn=1`、`MarketSign=8000`）、
`SSIDSyncRadio=1`（绕开 `bRadioStatusOn()`）、`P1905AcdBssCfg.CustomTag=68`、`CmWLANGetAPConfig` 对槽位 0 到 3 都成功、
每 BSS 的 `MapAccessAttr`（主 BSS 32）与 `ACLPolicy`（主/访客 1，回程 0）。

报文层面：
- 子路由只处理**它已经认识的**控制器发来的 Multi-AP 消息；在收到控制器的拓扑响应（Supported Service TLV = 控制器）并转
  `DEV_ONLINE` 之前，早到的 `0x8001`/`0x8003`/`0x8004` 被丢弃（日志 `query message 0x... is not from controller[..] ignore`）。
  控制器只在启动后约 25 到 80 ms 内发一次，不重发。控制器下线后子路由约 1 到 2 分钟才忘记它。【真机】
- 策略热更新只有一条可靠通道：原厂 CM 设置函数用的同步 `SSEND(0x2401, {u32 消息号, char name[32], u32 0})` →
  `multiapd.map.master`，消息号 `0x10206`（域/漫游阈值）、`0x10205`（主控策略）。直接 `sendcmd 1 DB set` 写库不会发它，
  daemon 既不重读也不推送；工厂调试通道 `/var/tmp/<进程>_asy` 也没有效果。见 [REPLAY_RESULTS](research/REPLAY_RESULTS.md)
  第 11 节与 [SERVICE.md](SERVICE.md)。
- 控制器的 `AL MAC` 默认 `02:00:00:00:19:06`；启动器的 `ZTE_MESH_AL_LAST` 改最后一个字节（实验时用来让子节点当成新控制器）。
  OpenWrt 包用 UCI `al_last`，默认取 LAN 桥 MAC 末字节。

## 5. 自组网 V2（`P1905ComCfg`）

### 5.1 它是什么
子路由 MESH 页有两个开关：**MESH 配置开关**（`MultiAPCfg.Enable`）和**自组网 V2**（`P1905ComCfg.Enable`，网页字段 `PEnable`；
MESH 关闭时不显示也不提交）。**它和 `VersionMode` 无关**：用户关掉 V2 后 `VersionMode` 仍是 0。界面里另有一个"v1/v2"，
是**拓扑页面的显示格式**，与这个开关无关。子路由当前值：`Enable=0`、`MarketConfEn=1`、`MarketSign="8000"`。

### 5.2 Z506（旧固件）：只有接收方向的准入检查【代码】
`multiapd` 里读这张表的只有 `baseGetCommDBCfg` → `baseInterComm`：
- `Enable==0`：直接放行，**不检查对方是谁**（包括非 ZTE 设备）。
- `Enable==1`：对未通过的对端要求带厂商 TLV（类型 `0x0b`）：没有 → 原因 3；厂商头无法识别或厂商名不同 → 1；
  `MarketConfEn==1` 且市场码不同 → 2；通过后记入"可互通设备"列表。日志
  `The device[mac] is not allowed to MESH with the current device,reason [n]`。
- 发送方向不按这个开关取舍：所有设备都带自己的厂商身份 TLV。

### 5.3 SR（新固件）：一整套互通控制【代码】
8 个字段（偏移 36、68、100、132、164、196、292、324）：`Enable`、`EnableIsSync`、`Policy`、`DropNum`、`MarketConfEn`、
`MarketSign`、`DropRoam`、`A1Enable`。三个关键位（字段语义与我们的取值见
[CONTROLLER_CAPABILITIES](CONTROLLER_CAPABILITIES.md) 第 H 节）：

- `Policy`：0 只记录不拦截、1 拒绝、2 "限制模式"（放行，但经 `ASEND 0xa94d` 通知无线管理模块，并按随机间隔丢弃该设备发来的
  `0x8014` Client Steering Request，上限由 `DropRoam` 决定，返回原因码 4）。
- `EnableIsSync`：主路由在策略报文里带厂商子 TLV `0x60`（`Enable` + `MarketConfEn`），子节点只有自己的 `EnableIsSync`
  也为 1 才接收并改写本机值。Z506 没有这个解析代码。
- `A1Enable`：WPS M1 型号名追加 `(A1)`；控制器侧要求对端型号名含 "A1"，否则返回 -1 拒绝。

未验证：`cspd` 收到限制事件后实际怎么限制、`DropNum` 单位、`A1` 的含义。

### 5.4 组合与我们程序的默认值
每方只按自己的开关检查对方，两个方向都通过才能组网：

| 控制器 | 子路由 | 结果 |
|---|---|---|
| 开 | 开 | 双向严格，需同为 ZTE 且市场码一致。【真机】项目早期的整套准入门槛就是在这个组合下从失败走到成功的 |
| 开 | 关 | 只有控制器检查子路由（它是 ZTE/8000，通过）。【真机，`mesh.v2=1`】子路由在线，识别为 `ZXHN Z506`、`zte:true` |
| 关 | 开 | 只有子路由检查控制器：需要控制器带 ZTE 身份和相同市场码。【代码】未在真机单独验证 |
| 关 | 关 | 谁也不检查，任何 1905 设备都能接。【真机，默认配置】 |

**我们默认 V2 关闭**（`mesh.v2=0` → 写控制器自己的 `P1905ComCfg.Enable=0`，启动日志 `p1905_v2=0`；`=1` 恢复严格检查；
改动需重启服务）。这只影响控制器**接收**时的检查；对外身份（ZTE / `ZXSLC SR1010`）和市场码（`1` / `8000`）不变，
所以子路由开着 V2 时仍接受我们。`EnableIsSync`、`Policy`、`DropNum`、`DropRoam`、`A1Enable` 保持默认 0。

## 6. `MultiAPCfg.VersionMode`：功能位掩码

不是 V2 开关，界面里没有入口。Z506 建表时写 0。从调用点后的立即数读出的位：

| 位 | 值 | 作用 |
|---|---|---|
| bit0 | 1 | 插件 BSS 配置（`acdInitPluginBssCfg` 等）；报文转发处理（`baseGetMsgForward`） |
| bit1 | 2 | **置位 = 偏标准 Multi-AP，不发 ZTE 私有厂商 TLV；清零 = ZTE 私有行为。** 发送端置位时不插入厂商 TLV；控制器置位时不主动发 AP 能力/信道偏好查询；`CombinedInfrastructureMericsSend` 置位发标准链路度量 TLV，清零发 ZTE 私有邻居表 TLV；`ChannelSelectResponseReceive` 置位时回 ACK |
| bit3 | 8 | Multi-AP Profile-2：WSC M1 里带 Profile-2 AP 能力 TLV；物理链路上下线处理 |
| bit7 | 0x80 | Wi-Fi 7 的 AP MLD / STA MLD 配置 TLV 编解码 |

协商：每台设备把自己的值放进 ACD 厂商 TLV；子节点收到 M2 或控制器报文时记下**控制器的值**，处理 M2 时已知就用控制器的，
否则用本机的——即子节点应用配置时**跟随控制器**。

**对我们**：`version_mode` 固定 128（bit1 清零 → 仍发 ZTE 私有厂商 TLV，子路由开着 V2 时的身份检查与 `CustomTag=68`
都在这条路径上；bit7 顺带打开 Wi-Fi 7 的 TLV）。早期用 0，现已统一到原厂基线 128——早期文档里的 0 已过时。
没验证：在真子路由上换成别的值；bit2、bit4 到 bit6 的含义。

## 7. CurHop（跳数）

- 控制器把自己的 `MultiAPCfg.CurHop` 原样放进厂商私有 TLV（子类型 `0x43`，大端 4 字节，在 Higher Layer Response 即
  CMDU `0x000e` 里）；`CfMultiapGetCurHop` 读库（偏移 260，失败返 -2）。子节点收到后：有线接入直接采用，无线接入减 1（≤0 则 -1）。
- 主路由的 `CurHop` 由 `slave_init` 设成 `MaxHop`，但 `multi_ap_slave_main` 在 `EnMasterOnly != 0` 时**跳过 `slave_init`**。
  我们的控制器 `EnMasterOnly=1`（SR1010 没有无线电，必须如此），`CurHop` 因此停在默认 -2，子路由日志
  `Get hop -2 from 02:00:00:00:19:06`。【真机】
- **修复**：`multiap-cfg.c` 启动时写 `CurHop=2`；真机验证子路由读到 2。

## 8. 子路由 Web 的"受控后锁定"

锁只有一个来源，是子路由本地标志，与主路由在不在线无关：`OpenAndSlave = (Mode == "Slave" && Enable == "1")`
（`globalenv_register.lua`，每次请求重算）。【代码】

**新版界面（Vue）**：主 Wi-Fi 配置与备用 Wi-Fi 配置 `isDisable = (OpenAndSlave != "false")`；射频配置
`isSlaveMode = (Mode == "Slave")`，2.4G/5G 开关 `OpenAndSlave == "true"`；MAC 过滤页的模式单选 `disabled = OpenAndSlave`。
**旧版界面（`webmodules` 的 `.lp`）**：高级 SSID 页输入框置灰；基本 SSID 页**只禁用"双频合一"，名称和密码没有锁定代码**；
`dmenu_modifier.lua` 的 `filterSlave` 作用在 WAN、LAN IPv4/IPv6、DMZ、端口转发、DDNS、UPnP、高级 Wi-Fi 等菜单项。

真机观察与规则一致。**任何控制器都不可能直接让某个页面变只读：它只能让子路由成为 Slave。**

## 9. ACL

- 同步方向：入网时子路由上报自己的 ACL，控制器回完整列表；子路由按列表删除多余、新增缺失（`CmSetAclCfg`，
  日志 `Del %d ACLs`）。控制器列表是权威的。请求 TLV：`0x1c` 频段号、`0x1d` SSID 数、`0x1b` ACL 数/上限；
  响应每条：`0x14` 接口、`0x11` 策略、`0x15` 名称、`0x16` MAC。
- 数据源 `AclCfg` 表（`ViewName` 主键、`Name`、`Interface`、`MACAddress`、`ACLCfgPolicy`；268 字节、256 行），
  控制器经 `CmWLANGetAclListValid` → `adevQueryAclCfgListByIf` 读，走数据库视图，不需要无线硬件。
- 策略值（`WLANCfg.ACLPolicy`）：0 关闭（驱动 `AccessPolicy=0`）、1 拦截名单内（`2`）、2 只放行名单内（`1`）。
  驱动是 Ralink 风格（`iwpriv ... set ACLAddEntry=` / `ACLClearAll=1` / `AccessPolicy=`），读回用
  `iwpriv wlan0 set ACLShowAll=1` 看内核日志（需要带 `dmesg` 的 busybox）。
- **Z506 的实际行为**：每条规则不分接口，都出现在 AP1、AP2、AP5、AP6 上（5G 主 SSID 名称带 `_5G`）；策略变化会连带改 AP3。
- **拦截已验证**：一加手机的 MAC 加入 `ban` 后驱动显示 `Policy is 2 : ... rejected`，站点立刻消失，删除规则后重新连上。
  **手机私有 MAC 会变**，同一台一加用过两个 MAC，只封旧的无效。
- 没验证：`allow` 策略、5G 驱动读回。
- 槽位：0 主 Wi-Fi、1 访客、2 备用、3 回程隐藏。AP 到接口：AP1 `wlan0`、AP2 `wlan1`、AP3 `wlan2`、AP4 `wlan3`、
  AP5 `wlan5g0`、AP6 `wlan5g1`。

## 10. 入网时的频段配置、信道、发射功率、带宽

- **FreqBand vendor 消息只有三个子 TLV**：`0x07` BeaconInterval（每频段，毫秒，4 字节大端）、`0x19` MaxHop、
  `0x55` STA 绑定支持标志；**没有 Fragment、TxPower、带宽、信道**（早先"有 Fragment/TxPower"是把控制器的调试打印
  误当成了报文）。Z506 只处理 `0x07`、`0x19`。BeaconInterval 写入 `DEV.WIFI.RD<n>`。【真机】
- **信道/功率**：标准 `0x8006`（TLV `0x8b` 信道偏好：运行类别 81/82（2.4G）或 115/118/121/124/125（5G）、一个信道、
  偏好 14；TLV `0x8d` 发射功率）→ 子路由 `slave_set_radio_cfg` 校验后写信道、功率和无线电使能并复位射频 → `0x8007`（响应码 0）。
  服务自己构造这个报文（`controller-packet.c`，源地址是 `br0` 的 MAC，消息号从 `0xe000` 起）。写信道会同时写
  `AutoChannelEnabled=0`；信道 0 只置 `AutoChannelEnabled=1`，所以还原要先发原信道再发 0。
- **带宽没有下发路径**：M2 和 FreqBand 都不带；`0x8006` 里带宽只作校验不写入。硬件 2.4G MT7592（20/40）、
  5G MT7613（20/40/80），不支持 160。【代码】
- 控制器自己的 `master_set_radio_cfg`（`0x10203`）需要代理的无线电清单，我们的 master 没有，所以服务直接发 `0x8006`。

## 11. 漫游阈值、频段引导、MLO

- **漫游阈值**：`roam.*` 写 `MultiAPDomain` 行，控制器发 ZTE vendor 策略 TLV（RCPI = (RSSI+110)×2），子路由回 CMDU 层 ACK；
  **Z506 没有解析代码，不生效**（`RoamRssiLmt` 一直没变）。早先"子路由确认并生效"的说法是错的。
- **标准 Metric Reporting Policy（`0x8a`）会生效**：子路由 `rssithr` 读回随之变化。公开零默认值曾把 STA 上报阈值推成
  RCPI 220，现在启动时设 -110。【真机】
- **频段引导/双频合一**：`band_steer.*` 只写控制器自己的 `EnBandSteer` 和 `BsRssiLmt24G/5G`（不在任何发给子路由的 TLV 里）；
  `wifi.dual_band_sync` 只让控制器的 5G 配置跟随 2.4G。实际引导行为未验证（需要有终端关联在子路由上）。
- **MLO**：SR 有完整 Wi-Fi 7/MLO 代码，Z506（Wi-Fi 5）一行都没有，无法验证。

## 12. 运行环境与 OSS/CM 事实

- 原厂 `multiapd` 要求：私有 `/var/tmp`、`/var/run`、`/var/log`（在 `/var/tmp/<task>_{asy,snd}` bind Unix socket）；
  128 KiB 订阅者池（`SubShmSrvInit`，key `0xabcb`）；`/dev/urandom`（旧 OpenSSL 只绑这一个设备）；`CONFIG_COMPAT` 的
  ARM32 原生执行。网络上在私有 netns 里：`veth` 对端，LLC `0x0004` 与 1905 `0x893a` 的 packet socket。
- 外部调 CM API 要用 `liboss_pub.so` + `InitAppComm`（像工厂 `diagget`），不能用 daemon 的 `liboss.so`；在 supervisor 里调会崩。
- 数据库启动视图（`startup-views.inc`，从 `cspd` 还原）：`P1905ComCfg`（328/8）、`IPInterface`（2404/48）、
  `P1905_ACD`（456/9）、`P1905AcdBssCfg`（264/5，8 行）、`AclCfg`（268/5，256 行）、`WLANWEP`（232/3）、
  `P1905DeviceInfo`（788/11，1024 行）。
- 控制器的 `MultiAPCfg` 夹具值（`multiap-cfg.c`）：`Mode=2`、`Enable=1`、`SyncEnable=1`、`SetRoleEnable=1`、
  `EnMasterOnly=1`、`CurHop=2`、`RetryTimes=4`、`MeshName=zte-shim-lab`、`IFLink=br-lan`；`VersionMode` 由服务配置给。
- 主控事件表（数据段 `0xe1520` 一带）：`0x10203` 无线电配置、`0x10205` 主控配置、`0x10206` 域配置、`0x10208` BTM 模式、
  `0x10216` 无线电信息、`0x10226` STA BTM、`0x10227` STA 断开、`0x10238` 代理频段引导。
- `ACLPolicy` 回归：控制器曾不设它，默认报 0，子路由把原来的 1 改成 0。现按子路由原值报告（主/访客 1，回程 0）。

## 13. 子路由（Z506）只读调试方法

telnet 进去：`sendcmd 1 DB all` / `sendcmd 1 DB p <表>`（`MultiAPCfg`、`P1905ComCfg`、`WLANCfg`、`WLANBase`、`AclCfg`、
`MapCooperatorCfg`、`WANCPPP` 等）、`sendcmd multiapd.map.slave dev|status|bss|sta|neighbor`（`dev` 含 `CurChannel`/
`CurTxpower`/`RCPISteerThr`/`rssithr`）、`sendcmd multiapd.<模块>.<模块> -l 8`（日志级别，重启后重置，用 `-l 7` 还原）、
`logctrl -r -g 1 -n`（跟日志，只给新行）、`iwconfig`/`iwpriv`。
`sendcmd` 报文：AF_UNIX 数据报发到 `/var/tmp/<进程>_asy`，0x224 字节，`+0x24` 目标名，`+0x48` 魔数 `0x11102`，
`+0x4c` 长度 `0x1c4`，`+0x60` 载荷。
子路由自带工具只有 `grep tr cut sed awk pgrep wget tftp`；没有 `head tail od dd sort dmesg`，`/var/tmp` 里的 busybox 重启即丢。
Web 代码在 `/home/httpd/webmodules`（Lua + `.lp`）和 `/home/httpd/public/jquery/static/js`（Vue 打包）。

## 14. 还没有查清的问题

1. 选举在"干净重启"下自行完成已有一次真机观察；"残留发布登记"的解释没有单独证明。
2. `Policy=2` 的限制在 `cspd` 一侧具体做什么；`DropNum` 单位；`A1` 的含义。
3. `VersionMode` 其余位；换值对真子路由的影响。
4. 更早一次"只锁备用和射频、不锁主 Wi-Fi"的原因。
5. 子路由 SSID 查询（索引 1 到 3）的真实应答（当前答"没有这个 SSID"，不影响 M2 路径）。
6. steering 等终端动作的触发路径；长稳；控制器异常退出后的恢复。
7. 一次干净的"控制器关 V2 + 子路由开 V2"组合的真机验证。
