# 真实子路由联调：已验证能力、配置语法与风险

SR1010 上的原厂 `multiapd`（Controller 角色，私有沙箱内）与一台真实 ZTE 子路由（ZXHN Z506，固件 V1.0.0.2B2.8000，
`192.168.5.216`，`d4:b7:09:0d:77:30`）联调的结论。协议层深入分析在 [FINDINGS.md](FINDINGS.md)，
构建/测试/守则在 [DEVELOPMENT.md](DEVELOPMENT.md)，控制器能下发什么在 [CONTROLLER_CAPABILITIES.md](CONTROLLER_CAPABILITIES.md)。
**我们从未改过子路由的模式**：它是用户设的"MESH 开、自组网 V2 关、角色自适应"（选举后 `Mode=1`、`SetRoleEnable=1`）。

## 1. 状态

真机通过：发现/拓扑/链路指标/Higher Layer（CMDU 0、1、2、3、5、6、7、13、14）、AP-Autoconfig + WSC M1/M2（真 DH、HMAC、
Key Wrap，子路由 `config M2[0]`）、SSID 与 WPA2 密钥落空口（用户确认 2.4G/5G 均为 `cc`）、重复入网与 Renew、
多 BSS 槽位 1 到 3、BeaconInterval、信道与功率（均已还原）、ACL 主 SSID、角色选举与 `CurHop`、自组网 V2 两种取值、
重启子节点与指示灯。细节与"未验证"的部分见第 4 节。

以下各项不生效，原因是 Z506 固件没有对应代码（不是我们没做）：漫游阈值（Z506 不解析 ZTE vendor 策略 TLV）、
带宽 20/40/80/160（没有下发通道）、`ForceWiFi5`（线上值正确，Z506 忽略）。频段引导/双频合一控制器参数已写入，
实际引导行为未验证。子路由 Web 的"受控后锁定"由它本地 `Mode`/`Enable` 决定，控制器只能让它成为 Slave（FINDINGS 第 8 节）。

未测：30 分钟以上长稳、控制器崩溃后恢复、用已安装的包接真子节点。LuCI/procd 服务化已在真 procd/rpcd 上用隔离网桥验证。

## 2. 入网的必要条件（缺一不可）

每一项都经反汇编确认，并在真机上从失败走到成功。

| 门槛 | 现象 | 做法 | 作用范围 |
|---|---|---|---|
| 设备身份 | 子路由 `reason=1` 拒绝 | `DevInfo.ManuFacturer/ModelName` 用 SR1010 的公开值（ZTE / `ZXSLC SR1010`），不含序列号 | 仅 `--controller-service`，环境变量 `ZTE_MESH_REAL_MANUFACTURER`/`ZTE_MESH_REAL_MODEL`；实验室仍用占位名 |
| 市场码 | `reason=2` 拒绝 | `P1905ComCfg.MarketConfEn=1`、`MarketSign` 与子路由一致（`8000`） | `ZTE_MESH_REAL_MARKET_SIGN` |
| 无线电状态 | M2 带 Tear Down | `MultiAPCfg.SSIDSyncRadio=1`（有线控制器绕过 `bRadioStatusOn()`） | 启动时无条件设置 |
| BSS 启用标记 | M2 带 Tear Down | `P1905AcdBssCfg.CustomTag=68`（0x44 = bit2+bit6；bit0 不置，启用位每次 Renew 从 `WLANCfg.Enable` 读，CM 适配层补 `Enable` 字节） | 每个 BSS 行 |
| 槽位应答完整 | M2 带 Tear Down（`bMapAccessAttr=16`） | `CmWLANGetAPConfig` 必须对每个频段的槽位 0 到 3 都成功；`acdGetWLANCFG` 遍历 4 个槽位，任一失败整体返 -1 | 无自有行的槽位返回 `InstExist=0`，不是错误 |
| 每 BSS 的 `MapAccessAttr` | fronthaul 与回程区分 | 主 BSS 行设 32（fronthaul） | `wlan-profiles.c` |
| 每 BSS 的 `ACLPolicy` | 见 §4 | 主/访客 1、回程 0，与子路由原值一致 | `wlan-profiles.c` |
| `VersionMode` | 决定是否发 ZTE 私有厂商 TLV | 控制器用原厂基线 128（bit1 清零才发私有 TLV；bit7 加 Wi-Fi 7 的 `0xdd`/`0xe0`） | 服务配置 `version_mode` |
| 控制器的 `CurHop` | 子路由日志 `Get hop -2` | 启动时写 `MultiAPCfg.CurHop=2`（等于 `MaxHop`） | `multiap-cfg.c` |
| 自组网 V2（`P1905ComCfg.Enable`） | 控制器是否严格检查邻居 | **默认 0（关）**，`mesh.v2=1` 开 | 服务配置；对外身份与市场码不受影响 |

真子路由与 SR1010 的 `multiapd`/`libcmapi.so` **不是同一个构建**（SHA256 不同），M2 由 SR1010 一侧构造：
控制器行为读 SR1010 的二进制，子路由能否接收读子路由的二进制。

## 3. 服务配置语法

服务读 `/run/zte-mesh/config`（`root`、`0600`、固定键、不允许重复；任何一行不合法整份被拒，日志 `config=rejected`、
`fail=config-missing-or-invalid`）：

```
version_mode=128                # 控制器的 MultiAPCfg.VersionMode；原厂基线 128
radio0.enabled=1                # 必填；0 会真的关掉子节点这个频段的主 SSID
radio0.ssid=<名称>
radio0.key=<8 到 63 个字符>
radio1.enabled=1
radio1.ssid=<名称>
radio1.key=<...>
radio0.hidden=0|1               # 可选，默认 0；热更新（只 Renew 该频段）——热更新路径没试过（会动用户真实的 Wi-Fi）
radio0.beacon_type=0|3|6|10|12  # 可选，默认 3（原厂基线 12）：0 不加密、3 WPA2、6 WPA+WPA2、10 WPA3、12 WPA3+WPA2
radio1.hidden=0|1
radio1.beacon_type=0|3|6|10|12

# 可选：自组网 V2（控制器自己的 P1905ComCfg.Enable）。默认 0；改动 = 写库并重启守护进程（不重启服务）：
# 子节点掉出拓扑约 8 到 58 秒再重新入网（15 次实测；原厂样本 41 秒）
mesh.v2=0|1

# 可选：设备角色。master = SetRoleEnable 0；auto = SetRoleEnable 1（路由模式下选举结果是 Master，Mode 仍是 2）。
# 不支持 slave（需要守护进程当 agent）；其它取值整份被拒。键的出现/消失需重启服务；
# master→auto 热更新 = 守护进程重启两次（相隔约 3 秒，原厂一样），auto→master 只清 SetRoleEnable、不重启。
# 不写这个键时库里保持固定值（Mode 2、SetRoleEnable 1）
mesh.role=master|auto

# 可选：额外 BSS 槽位 K（1 到 3），三项必须同时给出，缺一项整份被拒。同一无线电上必须是 1..k
# （槽位 3 不能没有槽位 2：M2 里 BSS 条目按位置对应，中间有空会把槽位 3 的设置套到槽位 2 上并关掉槽位 3）。
# 改 enabled 热更新（只 Renew 该频段）；增减槽位 = 写表后重启守护进程；已存在槽位的 ssid/key/hidden/beacon_type 热更新
radio0.bssK.enabled=0|1
radio0.bssK.ssid=<名称>
radio0.bssK.key=<8 到 63 个字符>
radio0.bssK.hidden=0|1          # 可选；默认只有槽位 3 为 1（原厂的隐藏回程 BSS）
radio0.bssK.access_minutes=<1..10080>  # 可选：访客限时。到时后有效使能位变 0（配置里仍是开）并 Renew 该频段；
                                # 关掉再打开 enabled、或改这个值会重新计时；被定时关闭压着时不计时；服务重启后重新计时
radio0.bssK.force_wifi5=0|1     # 可选，写 WLANCfg.ForceWiFi5（厂商查询子 TLV 0x46）；Z506 忽略
radio0.bssK.beacon_type=0|3|6|10|12  # 可选，默认 3。M2 给子节点的是它与 M1 所报类型的交集：Z506 没有 SAE，
                                # 12 到它那里等同 3；10（纯 WPA3）交集为空，Z506 忽略该项保持原值；0 是开放网络，
                                # M2 短 16 字节。只在停用的槽位 2 上验证过。密钥仍必须给

# 可选：Wi-Fi 定时关闭（服务里的调度器）。最多 8 条；有 timer.N.* 就必须给 timer.utc_offset（沙箱没有时区）
timer.utc_offset=<-720..840>    # 分钟，东为正
timer.N.start=HH:MM             # N = 1..8，必填；与 end 不能相同
timer.N.end=HH:MM               # 必填；end 不晚于 start 表示跨过午夜
timer.N.days=<7 位 0/1>         # 可选，默认 1111111（周一到周日，窗口"开始"的那一天）
timer.N.scope=all|guest         # 可选，默认 all：所有主 SSID 与槽位 1、2 的有效使能位置 0（隐藏回程槽位 3 不动）；
                                # guest：只关槽位 1。热更新。真机只验证了 guest（all 会断开真实 Wi-Fi）

# 可选：BeaconInterval（毫秒，20 到 1000；不给则 100，与子路由出厂一致），可热更新
radio0.beacon_interval=<20..1000>
radio1.beacon_interval=<20..1000>

# 可选：信道与发射功率（不写 = 不动子路由现有设置；写了在子路由上线约 15 秒后下发，可热更新）
radio0.channel=<0|1..13>        # 0 = 自动信道；其它 = 固定（子路由同时关掉自己的"自动信道"）
radio1.channel=<0|36..165>      # 5G：36 40 44 48 52 56 60 64 100..144 149 153 157 161 165
radio0.tx_power=<0..30>         # EIRP dBm，子路由映射到 5 档（见 §4）
radio1.tx_power=<0..30>

# 可选：MAC 过滤（ACL）。最多 16 条，N = 1..16；改文件后自动 Renew
acl.N.mac=<aa:bb:cc:dd:ee:ff>   # 必填；单播、非全零
acl.N.name=<名称>               # 可选，最多 20 字符 [A-Za-z0-9_.-]，默认 aclN
acl.N.radio=0|1|both            # 可选，默认 both
acl.N.bss=0|1|2|3               # 可选，默认 0（主 SSID）；>0 的槽位必须已配置 bssK.*
radio0.acl_policy=disabled|ban|allow   # 可选；不写 = 保持子路由原值（ban）
radio1.acl_policy=...
radio0.bssK.acl_policy=...             # 可选（该槽位必须已配置）

# 可选：双频合一 / 频段引导 / 漫游阈值 / 弱信号强制切换。改值是热更新（写库后发原厂 CM 通知）；
# 键的出现/消失需重启服务（启动时才准备策略域的行）
wifi.dual_band_sync=0|1        # 1：5G 名称和密码跟随 2.4G，此时不能再给 radio1.ssid/key（矛盾配置被拒）；radio1.enabled 仍要给。
                               # 0 或不写：省略的 radio1.ssid = radio0.ssid + wifi.essid_suffix（原厂规则），
                               # 省略的 radio1.key = radio0.key；派生名超 31 字节整份被拒。
                               # 改值 = 写 WLANSetting.DualBandSync 并 Renew 两个频段，不重启守护进程。
                               # **不会**同时关频段引导：原厂在同一步把 EnBandSteer 1→0，这里要自己写 band_steer.enabled
wifi.essid_suffix=<1 到 8 个可打印字符>   # 可选，默认 -5G；热更新。派生名在真子节点上没有试过（会改真实的 5G 主 SSID）
band_steer.enabled=0|1         # 写主控侧 MultiAPMaster.EnBandSteer
band_steer.rssi24=<-110..-30>  # 写 MultiAPDomain.BsRssiLmt24G / BsRssiLmt5G（两项同时给）
band_steer.rssi5=<-110..-30>
roam.rssi24=<-110..-30>        # 写 MultiAPDomain.RoamRssiLmt24G / RoamRssiLmt5G（两项同时给）；Z506 不解析，不生效
roam.rssi5=<-110..-30>
roam.force=0|1                 # 弱信号强制切换：MultiAPDomain.EnLegacyStaRoam（8 个域）+ MultiAPMaster.EnBtmKickoff/EnRoamKick
mlo.enable=0|1                 # WLANSetting.MLOEnable，默认 1；热更新（两个频段 Renew + 0200fe 推送）
```

**热更新与重启**：主 SSID/密钥、已有槽位 K 的 ssid/key/hidden/beacon_type、主 SSID 的 hidden/beacon_type、BeaconInterval、
信道/功率、ACL、`wifi.dual_band_sync`、`roam.*`、`band_steer.*`、`roam.force`、`mlo.enable` 随文件改动热更新（WLAN 类只 Renew
变化的频段；策略类写库后发 CM 通知，子节点收到新的 `PolicyConfigReq` 并在 2 ms 内 ACK）。
**`mesh.v2` 改动 = 写库后重启守护进程；`mesh.role` master→auto = 重启两次**（第二次是监督进程替 cspd 的 `cooperator_mgr`
做的模拟）。`version_mode`、策略键的出现/消失只打印 `..._changed restart_required=1`，需要重启服务。
槽位 `enabled` 与增减槽位也已热更新。
**未验证**：主 SSID 的 `radioN.enabled` 热更新（代码路径与槽位相同，但会动真实主 Wi-Fi）、主 SSID 的 hidden 热更新。

`mesh.v2` 的校验（`--no-attach` 自检）：`0`、`1` 接受（`ZTE_CONTROLLER_STARTUP p1905_v2=0|1`），`2`、空值、`yes`、重复键
整份被拒；不写则为 0。

## 4. 各功能的验证结论

**多 BSS**：子路由应用 M2 时会把每个频段所有已声明槽位的 SSID、密码、开关一并应用。槽位含义（本机子路由）：
0 主 Wi-Fi，1 访客，2 备用 SSID，3 回程隐藏网络；其他型号不一定相同。**永远不要向真机推占位值**——第一次用占位名覆盖了
子路由本地的访客 SSID 并把它打开了；现在每个槽位 `enabled/ssid/key` 三项必填。入网 vendor 阶段另有 SSID 查询通道
（索引 0 到 3），子路由只应用其中的开关和 ACL 策略，其余子类型报 `unknown sub type`（无害）。

**BeaconInterval**：启动时写入控制器 `WLANBase` 行，热更新时改行并 Renew 该频段。真机：2.4G 设 120 读回 120（5G、DTIM、
Fragment 不变），删掉该键热更新回 100；改它会让该频段射频重启约 4 秒。vendor 消息只带 BeaconInterval、MaxHop 和
STA 绑定支持标志，**没有 Fragment、TxPower、带宽、信道**。

**信道与发射功率**：服务自己构造 `0x8006` 从私有口发出，等 `0x8007` 应答（`response_code=0`）；子路由上线后等 15 秒再发
（更早的会被丢弃），失败最多重试 4 次，值变化或子路由重新上线时再发，键被删除时保持当前设置。真机：2.4G 信道 6 + 15 dBm
→ 读回 `Channel=6`、`AutoChannelEnabled=0`、`TxPower=3`；5G 44 + 20 dBm → `Channel=44`、`TxPower=2`。还原要先发原信道再发 `0`
（自动），功率设回最大档；值不变时不重启射频。非法值（2.4G 的 14、5G 的 50、功率 >30、非数字）整份被拒。
dBm → 档位：2.4G >19→1、16–19→2、12–15→3、8–11→4、其余 5；5G >21→1、20–21→2、16–19→3、10–15→4、其余 5。
**带宽没有下发路径**；硬件上限 2.4G MT7592（20/40）、5G MT7613（20/40/80），不支持 160。

**ACL**：规则写进控制器 `AclCfg` 表（服务预建 32 行并就地改，运行中不增删行），守护进程在入网/Renew 的 vendor 阶段做成
ACL 消息；策略是每 BSS 的 `WLANCfg.ACLPolicy`（0 disabled、1 ban、2 allow），子路由 `cspd` 映射到驱动 `AccessPolicy`
2/1/0。真机：启动配 1 条后表里出现，热更新加第 2 条再删光都同步；`disabled` 后 AP1 策略变 0，删键恢复 1。
Z506 的实际行为：规则不分接口（即使只给 `radio=0,bss=0` 也出现在 AP1/AP2/AP5/AP6 上，5G 主 SSID 名称带 `_5G` 后缀），
策略变化会连带改 AP3。拦截已在一加手机上验证（驱动 `iwpriv wlan0 set ACLShowAll=1` 显示 `Policy is 2 : ... rejected`）。
**手机的私有 MAC 会变**，封某台设备前先看它此刻实际用的 MAC。没验证：`allow` 策略、5G 驱动读回。

**漫游阈值与频段引导**：`roam.*` 写 `MULTIAPDOMAIN1`，控制器发 ZTE vendor 策略 TLV（RCPI = (RSSI+110)×2，配 `-62/-72`
时线上字节 `60 4c`），子路由只回 CMDU 层 ACK。**Z506 没有解析代码，不生效**；早先"子路由确认"的说法是错的。
标准 Metric Reporting Policy（`0x8a`）会生效（子路由 `rssithr` 读回随之变化），公开零默认值曾把 STA 上报阈值推成
RCPI 220，现在启动时设 -110。热更新手段是原厂 CM 设置函数那条同步 `SSEND(0x2401, {u32 消息号, name[32], len 0})`
（`zte-meshctl notify-master <id>...`），发出的是 `PolicyConfigReq`，与原厂线上帧逐字节相同。
`band_steer.*` 的 `BsRssiLmt` 不在任何发给子路由的 TLV 里，是控制器自己的判断阈值；Z506 没有 MLO 代码，无法验证 MLO。

**角色选举、V2、CurHop**（细节见 FINDINGS 第 3、5、7 节）：手动设 Slave 时选举不发生、照常入网；改成自适应后
`multiapd` 不重启则**子路由不发 Search**、组网失败，`sendcmd multiapd.base.base roleplay` 手动开始选举后 `Mode` 0→1 成功；
用户设好三项并**重启**子路由后自行选举为 Slave、`CurHop=2`、`P1905DevNum=2`，主/备用 Wi-Fi 与射频页被锁。
控制器 `mesh.v2` 取 0 或 1 都在线。**没验证**：子路由开着 V2 而控制器 `mesh.v2=0`（需要用户在子路由上打开 V2）。

## 5. 联调流程

1. 准备：确认局域网里除子路由和控制器外没有未登记的终端（`dev-service-smoke.py` 读网桥转发表，不在 `--expect-macs` 里的
   MAC 拒绝接入；手机连着子路由 Wi-Fi 时也在表里，断开后要等老化约 5 分钟）。放行未知 MAC 必须由用户明确同意
   （长期名单见 DEVELOPMENT 第 5 节）。
2. 运行：见 DEVELOPMENT 第 3 节；真机始终加 `--no-change`。
3. 触发重新入网：**不要 `kill` 子路由的 `multiapd`**（会留下残留状态）。用重启控制器、`zte-meshctl renew-acd <band>`
   或让用户重启子路由。
4. 判断成功（子路由 `logctrl -r -g 1 -n`）：`Tear down` 为 0、`bMapAccessAttr is 32`、`parse M2 success[0]`、
   `config M2[0]`（无变化）或 `config M2[1]`（已应用）。控制器侧：`state.json` 里节点 `online:true`、`model:"ZXHN Z506"`、
   `zte:true`；子路由 `sendcmd multiapd.map.slave dev` 里有 `02:00:00:00:19:06`。
5. 收尾：路由器上无残留 `/run/zte-mesh`、`/tmp/zte-mesh-svc`、`zmesh0`、`ns-run`、`db-engine-probe`、`tcpdump`；
   子路由配置读回并还原。

## 6. 风险与教训

- 向真机推配置会真实修改并持久化它的设置。每次测试前用 `config_z506.xml` 之类的备份确认原值，测后核对还原；
  该文件含明文密钥，不要提交或外传。
- 短时间连续重配 2.4G，子路由的 2.4G 出现过掉到隐藏的默认 SSID；把 SSID 改成别的再改回即可恢复。
- `iwconfig` 在刚重启或已关闭的接口上读回的 SSID 不可靠。
- 代码路径里"实验室"和"真机"必须分开：身份、市场码只在 `--controller-service` 路径通过环境变量开启。
- 不要在未确认语义前改 ACL 策略；不要切换子路由的 Mesh 模式；不要向真机推占位值。
- 完整坑清单见 DEVELOPMENT 第 6 节。

## 7. 未完成

1. 子路由开着 V2 且控制器 `mesh.v2=0` 的真机验证；`Policy=2` 的限制行为、`allow` 策略、5G 驱动 ACL 读回、槽位上的 ACL。
2. 关主 SSID 的路径（`radioN.enabled=0`、定时 `scope=all`）——会断开用户的 Wi-Fi，要先征得同意。
3. 终端动作（踢、封、引导）、拓扑页的终端列表与回程 RSSI、MaxHop/上报参数等小项。
4. 长稳、多个子节点、用已安装的 OpenWrt 包接真子节点。

## 附：槽位 1–3 的原厂基线（Z506 上复现）

每个无线电 4 个槽位：0 主 SSID；1 访客（`GuestSsid=1`、`VapIsolationEnable=1`、关闭、`BeaconType 12`）；2 关闭、
`BeaconType 6`；3 隐藏回程（开启、隐藏、`ACLPolicy 0`、`MapAccessAttr 64`、`BeaconType 6`）。复现用的服务配置
（`<KEY>` 为用户指定的密码，仓库里不写）：

```
radio0.bss1.enabled=0  ssid=ZTE-tsEG65_GUEST  beacon_type=12
radio0.bss2.enabled=0  ssid=cc2-wifi5         beacon_type=6
radio0.bss3.enabled=1  ssid=ZMESH-c4FsH5_5G   beacon_type=6  hidden=1
radio1.bss1.enabled=0  ssid=ZTE-tsEG65_GUEST  beacon_type=12
radio1.bss2.enabled=0  ssid=cc2-5Gc-wifi5     beacon_type=6
radio1.bss3.enabled=1  ssid=ZMESH-c4FsH5_5G   beacon_type=6  hidden=1
```

（每行都要带 `radioN.bssK.` 前缀和 `key=<KEY>`。）额外槽位的 `MapAccessAttr`（槽位 3 为 64，其余 32）和
`VapIsolationEnable`（只有槽位 1 为 1）由程序按原厂基线设置，不是配置项。**注意**：访客槽位开着时子节点读回的
`VapIsolationEnable` 仍是 0，隔离行为没有验证，不要当作和原厂一致。

## 附：子节点 `WLANCfg` 基线

`tools/child/childread.sh` 的输出（只有非密钥字段；8 行的 `KeyPassphrase` 都是用户指定的密码）。每次真机测试结束后
应与它逐行相同，`LEDCfg.EnableLEDCfg=1`：

```
AP1 InstExist=1 Enable=1 ESSID=cc              BeaconType=3 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=32 ACLPolicy=1
AP2 InstExist=1 Enable=0 ESSID=ZTE-tsEG65_GUEST BeaconType=3 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=1 MapAccessAttr=32 ACLPolicy=1
AP3 InstExist=1 Enable=0 ESSID=cc2-wifi5        BeaconType=6 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=32 ACLPolicy=1
AP4 InstExist=1 Enable=0 ESSID=ZMESH-c4FsH5_5G  BeaconType=6 ESSIDHideEnable=1 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=64 ACLPolicy=0
AP5 InstExist=1 Enable=1 ESSID=cc              BeaconType=3 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=32 ACLPolicy=1
AP6 InstExist=1 Enable=0 ESSID=ZTE-tsEG65_GUEST BeaconType=3 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=1 MapAccessAttr=32 ACLPolicy=1
AP7 InstExist=1 Enable=0 ESSID=cc2-5Gc-wifi5    BeaconType=6 ESSIDHideEnable=0 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=32 ACLPolicy=1
AP8 InstExist=1 Enable=1 ESSID=ZMESH-c4FsH5_5G  BeaconType=6 ESSIDHideEnable=1 VapIsolationEnable=0 GuestSsid=0 MapAccessAttr=64 ACLPolicy=0
```
