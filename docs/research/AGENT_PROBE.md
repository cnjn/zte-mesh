# 子路由（Slave）角色的判定性实验：M2 到底落到哪里

> 2026-10-29。这是对"能不能让装了插件的路由器同时当子路由"这一问题的**第一步判定实验**，只做静态分析，不运行原厂程序。
> 复跑：`python3 tools/probe/agent-surface.py`（加 `--trace` 打印全部直接调用）。输入是两个固定哈希的原厂二进制，脚本直接读运行包里那一份：
> `multiapd` `a0ba6bee…`、`libcmapi.so` `e19109de…`（后者是 `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` 里的那份）。
> 文中提到的 `tools/lab/` 实验脚本（`spark.c`、`elfx.py` 等）已从仓库移除，`tools/lab/enroller.c` 现在是根目录的 `enroller.c`。
> 协议层背景见 [FINDINGS.md](../FINDINGS.md)，控制器侧已验证能力见 [CONTROLLER_CAPABILITIES.md](../CONTROLLER_CAPABILITIES.md)。

## 1. 结论

1. **agent 控制面是现成的，而且门是运行时数据而不是编译期开关**：`multi_ap_slave_main`（0x627a8）在 0x62824 调用 `slave_init`，
   前面挡着一个 `ldrb r3, [r4, #0x1f]` + `cmp` + `bne`。改数据库里的 `MultiAPCfg.EnMasterOnly` 就能放它进来，不需要改二进制。【代码】
2. **M2 不走 `wlan_mgr` 事件，走 libcmapi 的 setter**。`_acdcRcvApAutoConfigWSCM2` → `acdcParseM2Msg` → `acdcConfigM2EncryptedSettings`
   → `MeshWlanSetAPCfgByBandAndIfIndex` → **`CmWLANSetAPConfig`**（密钥走 `acdcWlanSetPWKey` → `CmWLANSetPsk`）。【代码】
3. **setter 先写库、后通知 cspd，而返回值是通知的结果**。`_CmWLANSetAPConfig` 的顺序是 `WlanCfgDbRead` → **`WlanCfgDbWrite`**（0x14d2c0）
   → `CmMultiAPWlanRenewNotify`（0x14d7c0）→ `cm_wlan_send_cmapi_set_msg`（0x14d8d8）；`_CmWLANSetPsk` 同理（`WlanPSKDbWrite` 0x147720，通知 0x147c30）。【代码】

**所以判定结果是"便宜的那条路"**：没有 `cspd` 时数据库照样被写，只有"通知"这一步失败；而这一步的失败正是 setter 的返回值
（0x14d8dc `mov r1, r0` / `mov r5, r0`，0x14d92c `mov r9, r5`，结尾 0x14d9d4 `mov r0, r9`），所以不装替身的话，**配置写进去了但 agent 会向上报配置失败**。

## 2. 这对成本估计的修正

上一轮的分析把子路由的成本估成"再做一遍 `cspd` 侧的逆向加一个 wlan_mgr 事件翻译层"，**这个估计偏高了**。按现在的证据，缺的只有两块：

| 缺口 | 内容 | 量级 |
|---|---|---|
| 返回值替身 | 让 `CmSendMsg2MM`（libcmapi，**已导出**，0x33df8）在没有 cspd 时返回 0。和 `controller-route.c` 里替身 `SSEND(0xa96e)` 是同一套做法 | 几十行 C，一个符号 |
| 库 → hostapd 同步器 | 轮询/监听 `WLANCfg`、`WLANPSK`、`WLANBase`，把 SSID、密钥、安全模式、使能、隐藏、`MapAccessAttr`（32 fronthaul / 64 backhaul）落到 hostapd / UCI | 真正的工作量，但数据源已经全在库里 |
| 选举触发 | `cspd` 的 `MapCooperatorACDUpProc`/`V4AddrProc` 置 `g_TriggerSelectFlag` 后才调 `CfIeee1905RoleElect`。没有 cspd 就要像 `zte-meshctl renew-acd` 那样直接向 `multiapd.acd.acd` 发事件 `0x909b` | 复用现有 ctl |
| 无线电状态 | agent 启动和上报要读本机无线电（`slave_local_dev_load` → `MeshGetDevInfo`/`MeshWlanGetBaseCfgByBand`，`CmWlanGetStaProfile`、`CmWLANGetChannelList` 等）。控制器侧靠 `SSIDSyncRadio=1` 绕过的 `bRadioStatusOn()`，这里绕不过去，要按 iwinfo 如实回答 | 中等，且与硬件相关 |

## 3. agent 侧实际会发出的 ASEND（解出来的事件号）

| 函数 | 事件 | 说明 |
|---|---|---|
| `slave_init` | `0xA938` ×2、`0xA921` | `0xA938` 与 `controller-trace.c` 里已记录的设备上下线事件同号；另有 `CfMultiapSetCurHop` 调用（这就是控制器侧要手工补 `CurHop=2` 的原因） |
| `acdcConfigM2EncryptedSettings` | `0xA93F` | 配置应用后的通知 |
| `slave_set_radio_cfg` | `0xA221` | 信道/功率；这个函数同时也直接 `dbAPISetView`，所以信道功率同样是先写库 |

这些是异步 `ASEND`，没有 cspd 时只是无人接收，不影响库里的值。

## 4. 失败路径的性质

`acdcConfigM2EncryptedSettings` 在 0x75e14 调用 setter 后：

```
0x75e18  cmn r0, #9      ; -9 = 不适用，跳过
0x75e1c  beq  skip
0x75e20  cmp r0, #0
0x75e24  moveq r5, #1    ; 成功标记
0x75e28  beq  next
0x75e2c  b    <loop>     ; 其它值为失败，记日志后继续下一个频段
```

即：**不会整体中止**，但整函数的成功标记 `r5` 不会被置 1。所以表现会是"每个频段都写了库、但 M2 被当成应用失败"，对主控表现为入网/配置失败。【代码】

## 5. 尚未验证（要么要运行时，要么要真机）

1. `WlanCfgDbWrite`/`WlanPSKDbWrite` 在 `db-engine-probe` 的库上是否真的成功——`WLANCfg`/`WLANPSK` 的行已经由 `wlan-profiles.c` 写好并注册，但写入方向从没在这个库上跑过。
2. 无线电状态/能力查询（`CmWLANGetChannelList`、`CmWlanGetStaProfile` 等）在沙箱里的返回值要如实反映本机无线，否则 agent 上报的能力是空的，主控不会给它下发配置。
3. **选举触发**：`0x909b` 能否在无 cspd 的情况下让 agent 发 Search（控制器侧 `zte-meshctl` 已经证明同类事件通道可用，但这一条没试过）。
4. **驱动面**：每个频段 4 个 BSS、回程 BSS 的 4-address、AP+STA 同频并发。这不在二进制里，完全取决于目标路由器的芯片与 mac80211。
5. 主控路径的回归：`EnMasterOnly=0` 之后 `slave_init` 会跑，它会 `CfMultiapSetCurHop`，与现在 `multiap-cfg.c` 手工写 `CurHop=2` 的补偿要重新对齐。

## 6. 下一步（运行时实验，在 lab 机上做）

1. 复制一份服务路径，加 `agent` 模式：`Mode=1`、`EnMasterOnly=0`、`SetRoleEnable=0`，其余不动。
2. 加 `libzte_agent_mm.so`：只替身 `CmSendMsg2MM`，转发真实现后返回 0，日志记一次。
3. 打开 `controller-trace.c` 的 ASEND 全量记录（现在只记少数事件），并在 `dbAPISetView` 上打点，看 `WLANCfg` 的 8 行是否按 M2 被改写。
4. 对端用 `controller-packet.c` 的合成主控走完 Search→Response→M1/M2（M2 用现有校验器的公开测试值）。
5. 判据：`WLANCfg`/`WLANPSK` 读回与 M2 一致 → 同步器方案成立，可以进入"库 → hostapd"的实现；读回不变 → 回到事件翻译层方案。

## 7. 与产品范围的关系

[GAP_ANALYSIS.md](GAP_ANALYSIS.md) 第 1 节记录过 2026-10-17 的范围决定：**只做 AC（master-only），Slave 角色不算缺口**。
本文不改变那个决定的理由，只更新它的成本依据：Slave 不再是"重写 cspd"，而是"一个返回值替身 + 一个库到 hostapd 的同步器 + 无线电状态适配"，
剩下的不确定性集中在驱动和无线电状态查询上。

## 8. 2026-10-29 运行结果：M1/M2 走通，配置落库

第 6 节第 1、4 步已经在 lab 机上做完：`Mode=1 / SetRoleEnable=0 / EnMasterOnly=0` 分支存在，
`controller_agent_startup()` 用 `controller-packet.c` 的合成主控陪 daemon 走完
Topology Discovery → Query → AP-autoconfig Search → Search Response → M1 → M2。

### 8.1 让 M2 被接受需要的四处帧形状

| 位置 | 原来（错） | 现在（对） | 依据 |
|---|---|---|---|
| Supported Service TLV (0x80) | 单条、值 1 | 单条、值 **0**（= Controller） | `I5TlvSupportedServiceTypeInsert` 0x97270：角色字节 0→Controller、1→Agent、其它→两条 |
| M2 里的无线标识 | TLV **0x85**（AP Radio Basic Capabilities） | TLV **0x82**（AP Radio Identifier），值取 M1 的 0x85 payload 前 6 字节 | `_acdsRcvApAutoConfigWSCM1` 用 `acdGetApRadioBasicCapabilitiesType` 取头部 6 字节，再 `I5TlvApRadioIdentifierTypeAcdInsert` 插 0x82 |
| M2 的 WSC 属性 | 0x1047（UUID-E） | **0x1048**（UUID-R），并补齐 M1 的回显项 0x1004/0x1010/0x100d/0x1008/0x1021/0x1023/0x1024/0x1042/0x1054/0x1011/0x103c/0x1002/0x1009/0x1012/0x102d | `eap_wps_process_message_M2` 固定顺序读这组属性，缺哪个报哪个（先报 `get WPS_TYPE_UUID_R error`，再报 `Check M1 WPS_TYPE_MODEL_NAME length=0`） |
| 0x1018 明文结构 | 外面套一层 Credential(0x100e) | **平铺**：0x1045/0x1003/0x100f/0x1027/0x1049 + 末尾 0x101e | `acdcParseM2Msg` 用一张 8 项表按属性号派发（0x1045、0x1003、0x100f、0x1028、0x1027、0x1020、0x101e、0x1049），不认 0x100e 外壳 |

加密格式也按 `eap_wps_build_message_M2` 对齐：0x1018 = **随机 16 字节 IV + AES-128-CBC(PKCS#7 填充)**，
填充量恒为 `16 - len%16`（对齐时补满一整块），与 `eap_wps_process_message_M2` 里
"buf 是 IV、buf+16 起是密文、最后一个解密字节必须 1..16" 的读法一致。

### 8.2 daemon 侧的实际动作序列

```
all M2 total Length: 668
The WSC-0 will cfg to 0 0
fail, mesh sync for M2: index[0], SSID[zte-agent-m2], Auth[0x20], EncryptionType[0x8]
acdcParseM2Msg cfgChg[0]=0
```

- SSID/认证/加密都从 M2 正确解出 → WSC 层（鉴权码、Key Wrap、Credential）全部通过。
- `acdc current state[0]:1,event:2 → 2,event:7`：状态机在两频段上都推进了。

### 8.3 第 6 节第 5 步的判据：**WLANCfg 读回与 M2 一致**

用两个不同的 profile 隔离变量：启动身份写 `zte-controller-test`（`verify_profile_adapter` 会断言这两个值，
不能动），M2 里给 `zte-agent-m2`。运行前后直接读库：

```
before  [0 ssid=zte-controller-test …][1 ssid=zte-controller-test …]
after   [0 ssid=zte-agent-m2        …][1 ssid=zte-controller-test …]
```

0 号行变成了 M2 的 SSID，1 号行不变（这一轮 M1 是 band 0 的）。
所以 **WLANCfg 的写入方向在这个库上确实成立**，同步器方案不需要退回事件翻译层。

`WLANPSK` 仍为空，原因是下一步而不是写法问题：

```
CmWLANSetAPConfig instid=DEV.WIFI.AP1 status=-3 errno=9
```

`acdcConfigM2EncryptedSettings` 的顺序是 `MeshWlanSetAPCfgByBandAndIfIndex`（写库成功，随后
`cm_wlan_send_cmapi_set_msg` 因为没有 cspd 返回 -3）→ `acdcWlanSetPWKey` → `MeshWlanMapSsidCfgSync2Repeater`
（NUL 参数直接返回 -1，所以日志是 `fail, mesh sync`）。AP 配置 notify 失败后 PSK 那一步没有执行，
`CmWLANSetPsk` 一次都没被调用（这一轮的 interposer 里是 0 次）。

> 更正（见 9.3）：这一轮观测里 `WLANPSK` 的 `key_len=0` 还有第二个原因——`dbGetValComm()` 按行号读这张表
> 读不到值，必须用 `dbAPIGetView()` 按实例 id 读。也就是说"空"这个观测本身当时是失真的；真正的门槛确实
> 只是返回值，但读法也要改，否则下一轮又会得到假的 0。

### 8.4 因此真正的下一道门槛

第 6 节第 2 步（`CmSendMsg2MM` 替身返回 0）从"可选"变成"必须"：它决定 `CmWLANSetAPConfig` 返回 0 还是 -3，
而 PSK 写入和 repeater 同步都挂在它的返回值后面。数据库写入本身不依赖 cspd（写库在 notify 之前），
这一点本轮已经用读回值证明。**该门槛已在第 9 节跨过。**

## 9. 2026-10-29 第二轮：PSK 与 repeater 同步落库，判据全部成立

### 9.1 替身的作用点比第 6 节写的更窄

真正需要的不是 `CmSendMsg2MM` 的全局替身（它有 324 个调用点，读接口拿到未填充缓冲区就会出错），
而是**只覆盖两个 setter 的返回值**：

```
ZTE_TRACE_VALUE text=CmWLANSetAPConfig instid=DEV.WIFI.AP1 status=-3 report=0 errno=9
ZTE_TRACE_VALUE text=CmWLANSetPsk      instid=DEV.WIFI.AP1.PSK1 status=-3 report=0 errno=9
```

厂商的返回码仍然照原样记在 `status=`，`report=` 才是交给调用方的值；开关是 flag 文件
`/run/zte-mesh/setter-ok`（沙箱内不可能用环境变量），不建就完全是原厂行为。

调用链的门槛位置来自 `acdcConfigM2EncryptedSettings`（0x75b7c）：

```
75e14  bl MeshWlanSetAPCfgByBandAndIfIndex   ; 原样返回 CmWLANSetAPConfig
75e18  cmn r0,#9  / beq                  ; -9 视为"已配置"
75e20  cmp r0,#0  / moveq r5,#1 / beq    ; 0 视为成功，进入下一步
75e2c  b 75f9c                           ; 其它（-3）直接失败返回
...
75e6c  cmp sl,#0  / bne 75fcc            ; sl = M2 flag bit4：置位时跳过 PSK
75e84  bl acdcWlanSetPWKey               ; → CmWLANSetPsk
```

注意 `75e6c` 那道 `sl` 判断：M2 携带"整段配置"标志时 PSK 是**按设计**跳过的，与返回值无关。
本轮 M2 没有置该位，所以走到了 PSK。

### 9.2 运行结果

```
success, mesh sync for M2: index[0], SSID[zte-agent-m2], Auth[0x20], EncryptionType[0x8]
acdcParseM2Msg cfgChg[0]=1
```

`mesh sync` 由 `fail` 变成 `success`，`cfgChg` 由 0 变成 1——上一轮这两行都是失败的。
（`CmWLANMapSsidCfgSync2Repeater` 的 interposer 没有命中，是因为 libcmapi 内部是直接调用而不是走 PLT，
只能靠这行日志判断。）

### 9.3 第 6 节第 5 步的判据：**两张表都与 M2 一致**

`WLANPSK` 必须用视图 API 按实例 id 读（`dbAPIGetView("WLANPSK", "DEV.WIFI.AP1.PSK1", …)`，密钥在行内偏移 260）；
用 `dbGetValComm()` 按行号读这张表读不到任何东西，上一轮的 `key_len=0` 是这个原因，不是没写进去。

```
before  DEV.WIFI.AP1.PSK1 key_len=20 m2_match=0   DEV.WIFI.AP5.PSK1 key_len=20 m2_match=0
after   DEV.WIFI.AP1.PSK1 key_len=18 m2_match=1   DEV.WIFI.AP5.PSK1 key_len=20 m2_match=0
        WLANCfg row0: zte-controller-test → zte-agent-m2
```

`m2_match` 是与 M2 里那把密钥的逐字节比较（只输出布尔值，不输出密钥）。所以：

- **WLANCfg 与 WLANPSK 两张表都写成了 M2 的内容**，且只有 M2 涉及的 band 变了；
- 同步器方案的数据源在真实 daemon 上完整可用，不需要退回事件翻译层；
- 主控路径回归无变化（`running=1`、启动断言全过、`nodes=[]`），主机 C 测试 162/331 断言全过。

### 9.4 剩下的工程项

1. `CmSendMsg2MM` 的**正式**替身（而不是 lab 的返回值覆盖）：把 set 消息直接落到库 + 返回 0，让 notify 链路在无 cspd 时也自洽。
2. `libzte_agent_mm.so`：把上述替身做成插件自己的库，而不是测试用的 preload。
3. 库 → hostapd 同步器：`WLANCfg`/`WLANPSK`/`WLANBase` 的变更落到 UCI/hostapd。
4. UCI/init/LuCI 暴露 `mesh_role=slave`（`openwrt/zte-mesh/files/zte-mesh.init:337` 目前只接受 `master|auto`）。



## 10. 2026-10-29 第三轮：band 1 与重启

第 9 节之后还剩两个运行时问题：**band 1 的第二份 M2 会不会被拒**，以及**配置跨重启是否保留**。
这一轮把两个都测了。

### 10.1 一个 radio 一份 M1，一份 M2 —— band 1 不会被拒

之前的观察循环只回答第一个 M1（`&& !m1_seen`），所以 band 1 一直停在 identity 配置上。
daemon 实际上**每个 radio 各发一次 M1**：

```
ZTE_PACKET_TLV daemon cmdu=0x0009 bytes=587 tlvs=11:539,85:17   # 0x85 前 6 字节 = "MAP24G"
ZTE_PACKET_TLV daemon cmdu=0x0009 bytes=617 tlvs=11:539,85:47   # 0x85 前 6 字节 = "MAP5GL"
```

改成按 radio 标识去重、逐个回答（最多 4 个 radio），并且**每个频段给不同的 profile**——
否则"第二份 M2 落到了正确的行"和"同一行被写了两次"在数据库里看起来一样：

```
after  WLANCfg [0 ssid=zte-agent-2g][1 ssid=zte-agent-5g]
       WLANPSK AP1.PSK1 key_len=16 m2_match=1 | AP5.PSK1 key_len=16 m2_match=1
       m1_seen=2 answered=2 band_24g=1 band_5g=1
```

daemon 侧两份 M2 的处理完全对称，没有任何一次 `Check APRadioIdentifier failed`：

```
The WSC-0 will cfg to 0 0   → CmWLANSetAPConfig DEV.WIFI.AP1     → CmWLANSetPsk AP1.PSK1
                            → success, mesh sync for M2: index[0], SSID[zte-agent-2g]
                            → acdcParseM2Msg cfgChg[0]=1 → state[0]:2,event:7
The WSC-0 will cfg to 1 0   → CmWLANSetAPConfig DEV.WIFI.AP5     → CmWLANSetPsk AP5.PSK1
                            → success, mesh sync for M2: index[0], SSID[zte-agent-5g]
                            → acdcParseM2Msg cfgChg[1]=1 → state[1]:2,event:7
```

两个细节值得记下来，它们解释了为什么"index"看上去一直不动：

- `The WSC-%d will cfg to %d %d` 的第一个数是**这份 M2 内部的凭据序号**，不是频段。
  每份 M2 只带一份凭据，所以两次都是 `index[0]`；频段来自 0x82 AP Radio Identifier TLV。
- acd 状态机是**每个频段一台**：`state[i]:0` → `1`(event 2) → `2`(event 7)，i = 0/1。

### 10.2 重启：库保留，磁盘不写，agent 每次重新入网

方法：`controller_respawn_daemon()`——SIGKILL 后重新 fork/exec，数据库与订阅池留在本进程里，
这就是工厂 `multiapd_mgr` 的 restart（真机上库在 cspd 里，也只有 daemon 会消失）。

第一次跑出来是 `reboot_m1=0`，很容易误读成"它记得自己已经配置过了"。实际不是：
新 daemon 构建了 M1 报文（`acdCreatWpaApConfileForM1` / `Build M1 phrase`）却**一帧都没发出去**，
因为 acd 任务不会自己离开 state 0——需先有一次拓扑交换把它推起来。补上"重启后再自我通告"后：

```
respawn status=0
reboot-before  WLANCfg [0 zte-agent-2g][1 zte-agent-5g]  PSK m2_match=1/1
reboot-after   WLANCfg [0 zte-agent-2g][1 zte-agent-5g]  PSK m2_match=1/1
wsc_m1_after_reboot bytes=587 mid=0xcbf3 band=0 answered=0
wsc_m1_after_reboot bytes=617 mid=0xcbf4 band=1 answered=0
```

加上两项旁证：

- `find /tmp/zte-mesh-svc/root -newer <marker> -type f` → **0 个文件**：daemon 从不把配置写盘。
- 重启后 acd 又从 `state[0]:0,event:0` 开始。

所以三件事要分开说：

| 问题 | 答案 |
|---|---|
| 库里的行跨 daemon 重启还在吗 | **在**（`reboot-before` 与 `reboot-after` 一致，`m2_match=1`） |
| daemon 会把它写到磁盘吗 | **不会**（0 个文件被写）。真机上这一步是 cspd 的职责 |
| 重启后 agent 还认为自己已配置吗 | **不会**（acd 从 0 起，重新发全部 M1，未回答还会重试） |

对插件的含义：子路由重启后**必须能接受重复下发的 M2**，不能假设"配置一次就够"；
反过来说，控制器侧只要在拓扑发现后无条件重发即可，不需要先判断对端是否已配置。

另外每份 M2 落库后 daemon 都会调 `CmMultiAPWlanRenewNotify`，这里稳定出现
`CmMultiAPWlanRenewNotify ASEND error !`——订阅者是 cspd，lab 里没有，所以 ASEND 失败。
这条 notify 正是"库 → hostapd 同步器"该挂的钩子（见 9.4 第 3 项）。

### 10.3 复现与回归

- 同一次实验连跑两次，两份 M2 都落在同一组行（`row0=zte-agent-2g`、`row1=zte-agent-5g`，两把 key 都 `m2_match=1`）。
- 主控路径回归无变化：`running=1`、`cfg mode=2 set_role_enable=1 en_master_only=1 cur_hop=2`、`startup_renew=0`、无 `fail=`。
- 主机测试 162 + 331 + 87 断言全过（packet / wsc / wsc-crypto），bridge 与 cm-profile 也过。

## 11. 2026-10-29 第四轮：把"能跑"变成"能装"

第 10 节之后剩下的都是工程缺口，这一轮按依赖顺序做完了三项：正式的配置管理器替身、库 → hostapd 的
同步器、以及在 UCI/init/LuCI 上暴露 `mesh_role=slave`。

### 11.1 `libzte_agent_mm.so`：替身从 flag 文件搬进运行时

上一轮的 setter 开关是**实验装置**：只要 `/run/zte-mesh/setter-ok` 存在，任何失败的 setter 都报 0。
这一轮换成 `agent-mm.c` → `libzte_agent_mm.so`，判断条件也换成了可验证的：

```
真实调用 → 只接管 CmWLANSetAPConfig / CmWLANSetPsk
         → 只有返回值 == -3（配置管理器不在）时，才去读回刚写的那一行
         → 读回值必须等于调用方传入的值，否则原样返回 -3
```

读回用 `dbAPIGetView()` + 两个已知偏移：CM 结构里 SSID 在 444、密钥在 188；库里分别在 708 与 260
（`cm-profile-layout.inc` 的 708→444 映射与 `controller-profile.c` 的 `q.data + 260` 是同一份证据）。
于是它的声明是"库已经是你要的值，只是推给一个不存在的管理器失败了"，而不是"假设它成功了"。

lab 证据（不再创建任何 flag 文件）：

```
ZTE_AGENT_MM setter=CmWLANSetAPConfig instid=DEV.WIFI.AP1    status=-3 row_applied=1 report=0 errno=9
ZTE_AGENT_MM setter=CmWLANSetPsk      instid=DEV.WIFI.AP1.PSK1 status=-3 row_applied=1 report=0 errno=9
ZTE_AGENT_MM setter=CmWLANSetAPConfig instid=DEV.WIFI.AP5    status=-3 row_applied=1 report=0 errno=9
ZTE_AGENT_MM setter=CmWLANSetPsk      instid=DEV.WIFI.AP5.PSK1 status=-3 row_applied=1 report=0 errno=9
```

`CmSendMsg2MM` 仍然**没有**被接管：324 个调用点，全局返回 0 会让读接口拿到未填充的缓冲区。
`CmWLANMapSsidCfgSync2Repeater` 也没有：libcmapi 内部直接调用不走 PLT，替身根本命中不到。
`/run/zte-mesh/mm-off` 可以让它退回原厂行为，用于对比诊断。

### 11.2 同步器：`agent-sync.c` + `wlan-sync`

`CmMultiAPWlanRenewNotify` 的订阅者是 cspd 与驱动层，两者都不存在，所以同步器不去拦截那条 notify，
而是**反复读它宣告的那两张表**（`WLANCfg` 的 ESSID/Enable/ESSIDHideEnable/MapAccessAttr/BeaconType，
`WLANPSK` 的 密钥），变了就发布一份计划：

```
ZTE_AGENT_SYNC layout essid=708 enable=388 hide=964 access=2020 beacon=900 key=164 passphrase=260 record2=64 record3=96
ZTE_AGENT_SYNC published=1 path=/run/zte-mesh/wlan-plan rows=2 plan_bytes=519 \
  [0/0 ssid=zte-agent-2g enabled=1 beacon=3 key_len=16] [1/0 ssid=zte-agent-5g enabled=1 beacon=3 key_len=16]
```

两个偏移上的教训值得记下来：

- 字段偏移不是硬编码，而是按 `wlan_cfg.def` / `wlan_psk.def` 的顺序用 `dbGetPmTypeValLen()` 走出来的
  （与 `controller-profile.c` 的 `read_query()` 同一算法）。首次读库会打一行 `ZTE_AGENT_SYNC layout`，
  厂商改了布局会立刻暴露。
- **`PreSharedKey` 不在 260**：按定义走出来的顺序它是 164，260 是 `KeyPassphrase`；ASCII 口令落在
  `KeyPassphrase`，而 libcmapi 的 `CmWLANGetPsk` 也是从 260 取的。所以先取 `KeyPassphrase`，为空再退回
  `PreSharedKey`（64 位十六进制那种）。

计划文件（`/run/zte-mesh/wlan-plan`，0600，`rename()` 原子替换）是纯行格式，每行 `key=value`：

```
rows=2
0.radio=0
0.slot=0
0.instance=DEV.WIFI.AP1
0.psk_instance=DEV.WIFI.AP1.PSK1
0.ssid=zte-agent-2g
0.key=m2-public-2g-key
0.enabled=1
0.hidden=0
0.access=0
0.beacon_type=3
```

`openwrt/zte-mesh/files/wlan-sync` 是沙箱外的另一半：按 band 找 radio 段、找该 radio 上主 AP 的
`wifi-iface`，把 `ssid/key/encryption/hidden/disabled` 写进 `/etc/config/wireless`，只在真的变了时才
`uci commit` + `wifi reload`。安全模式按 `BeaconType` 映射（0 none、3 psk2、6 psk-mixed、10 sae、12 sae-mixed）；
不认识的模式**不动**那个接口并记一行日志，而不是猜。额外 SSID 槽位（slot 1..3）只记录不应用：
那部分在真机上由原厂驱动接口负责，在这里造 hostapd 段只会产生第二个真相源。

服务侧挂在 `controller_service()` 的 slave 分支上（每秒一次，幂等），`--agent-startup` 的观测循环里
每 2 秒一次，退出前再发布一次。主控角色完全不调用它（`ZTE_AGENT_SYNC` 一行都不会出现）。

### 11.3 `mesh_role=slave` 暴露到 UCI/init/LuCI

- `zte-mesh.init`：`mesh_role` 接受 `master|auto|slave`；slave 时额外起一个 `wlan-sync` 的 procd 实例
  （`/usr/libexec/zte-mesh/wlan-sync /run/zte-mesh/wlan-plan`），`stop_service()` 顺带删掉计划文件，
  免得下次启动把上一次的角色配置再套一遍。
- `zte-mesh.config` 注释与 LuCI（`zte-mesh.js`）加了"子路由"选项，说明里写清两件事：子路由的 Wi-Fi
  名称/密码/加密来自主控；子路由**仍需一套自己的可用配置用于启动**（`set_text()` 不接受空值，
  主控下发之前就用它）。
- 服务本身早就认 `mesh.role=slave`（`controller-service.c` 的 `parse_config` + `apply_new_config` +
  主循环跳过 controller 功能），这一轮只是把它接到用户能按的地方。

生产路径的 smoke test（`--controller-service` + `mesh.role=slave`，lab 里没有主控对端）：

```
ZTE_CONTROLLER_STARTUP role=slave mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2
ZTE_MESH_SERVICE running=1 version_mode=128
ZTE_MESH_SERVICE role=slave controller_features=off
ZTE_AGENT_SYNC published=1 ... [0/0 ssid=child-boot enabled=1 beacon=3 key_len=14] [1/0 ssid=child-boot-5g ...]
ZTE_MESH_SERVICE startup_renew=skipped role=slave
```

即：子路由上电后先把自己的 bootstrap 配置发布出去，主控下发 M2 后再发布第二次。

### 11.4 测试与回归

| 测试 | 覆盖 | 结果 |
|---|---|---|
| `tests/agent-sync-codec.c`（新） | 计划文本（`=` 与制表符/换行的转义、不可读行、容量不足）、变化检测（SSID/密钥哈希/使能/BeaconType/hidden/行数） | 通过 |
| `tests/test_wlan_sync.sh`（新） | 用带 `uci` 语义的桩跑 `wlan-sync`：两个 band 的映射、幂等（第二次不 commit 不 reload）、关闭/open 网络、槽位跳过、未知加密模式不动 | 21 项通过 |
| `tests/test_init.sh` | 新增 `role-slave` 用例；把 `mesh_role slave` 从"拒绝"列表移出（改成 `child`） | 66 项通过 |
| `controller-*-codec`、`bridge-codec`、`cm-profile-codec` | 未改动路径 | 162 / 331 / 87 等断言全过 |

主控路径回归：`running=1`、`cfg mode=2 set_role_enable=1 en_master_only=1 cur_hop=2`、`startup_renew=0`、
无 `fail=`，且**不产生** `wlan-plan`（同步器只在 slave 跑）。

### 11.5 仍然缺的

- **驱动层**：`wlan-sync` 落的是 UCI + `wifi reload`。回程 BSS 的 Multi-AP 标记已经接上（§11.6），
  但 4 地址是否真的在驱动里生效只能在真机上看（`ip -d link` 的 `4addr on`）。
- **`CmWLANMapSsidCfgSync2Repeater`** 仍然执行不到（no-op），它在真机上把 SSID 推到 repeater 侧。
- **`CurHop`**：slave_init 自己会写；lab 里用 `-2` 顶替，真机上要确认它被真正更新。
- 子路由的 `wlan-sync` 只处理主 BSS 和回程 BSS；额外槽位（1、2）需要一套和原厂驱动接口对齐的方案。

### 11.6 2026-10-29 第五轮：回程 BSS 走 Multi-AP，不走 802.11s

第 11.5 节剩下的第一项是"驱动层 backhaul 去哪里提取"。这一轮先把结论定下来，再按结论实现。

**结论：不去原厂提取，用 hostapd 的 multi_ap。** 理由不是"提取太难"，是"前提不成立"。参考
`Zlion-Y/e2631-openwrt`（同一套运行时在 E2631 上的移植）的实测材料：

- 原厂无线是 `CONFIG_CSP_WLAN=y` + `# CONFIG_CFG80211 is not set` / `# CONFIG_WLAN is not set`
  （`docs/v1.5-驱动对齐与逆向移植报告` §1，读的是原厂 `/proc/config.gz`）。原厂 `mt7916.ko` 5.5 MB，
  自有 netdev + dat 文件，**完全游离于标准 Linux 无线栈之外**，mt76 与它不共享任何配置语义。
- 原厂的 mesh 参数确实在 dat 里（`board/vendor-runtime-observe-2026-10-28.out.txt`：
  `MeshId=`、`MeshAuthMode=`、`MeshWPAKEY=`、`MeshAutoLink=0`、`MapMode=0`、`INDEX0_mesh_ifname=mesh;meshx`），
  但**值是空的、`MapMode=0`**——原厂在这台机器上根本没启用 MTK 的 mesh，真正干活的是 ZTE 自己的 1905。
- 那批 dat（`mt7916.dbdc.b0/b1.dat`，7.3 KB × 2）在移植时只被 `ls` 出来过，没拉取；即使拉下来，
  mt76 也不消费（同报告 §3 的"剩余差距"）。

所以在 mt76 平台上"从原厂提取回程"没有落点。**也选了 multi_ap 而不是 802.11s**，这一条是技术性的：
1905.1 CMDU 是以太网帧，要跨无线就得走 **4 地址**帧；802.11s 的数据帧带 mesh header，CMDU 跑不上去。
Multi-AP 的 backhaul BSS 正是 4 地址 BSS，正好是这套控制面缺的那条数据面。

#### 实现

`wlan-sync` 多了一个模式，开关是 `zte_mesh.main.backhaul`（默认 `off`，此时脚本行为与第 11.2 节一字不差）：

| 值 | 行为 |
|---|---|
| `off`（默认） | 只应用主 BSS；plan 里的回程行只记一行日志；上次留下的 `zte_backhaul_*` 段与 `multi_ap=1` 会被撤掉 |
| `multi_ap` | 主 BSS 写 `multi_ap=1`（fronthaul）；plan 里 `MapAccessAttr & 64` 的行写成独立段 `zte_backhaul_<band>`，`multi_ap=2`（backhaul） |

几个刻意的取舍：

- **按 `MapAccessAttr` 的位判断，不按 slot 号。** 槽位含义各型号不同（REAL_CHILD §… 记录了 Z506 的
  0/1/2/3），而 32/64 这两位是 `wlan-profiles.c:305` 与子节点原值一致的通用约定。
- **没有回程行就不造。** SSID 和密钥全部来自主控下发的行；plan 里没有 `access=64` 的行时，本节
  什么都不创建，只让主 BSS 带上 fronthaul 标记。不在脚本里发明 `ZMESH-xxxx` 这类名字。
- **回程段的网络跟随主 SSID**（`network` 从主 BSS 抄），不硬编码 `lan`，免得和镜像自己的桥名打架。
- **`off` 只撤自己写的东西**：`multi_ap=1` 会清（这是本脚本唯一会写的值），`multi_ap=2/3` 留着
  （那是别人设的）。`ap_iface_of()` 同时排除 `zte_backhaul_*` 和 `multi_ap=2`，否则第二次运行会把
  回程段当成主 SSID。

#### 测试

`tests/test_wlan_sync.sh` 从 21 项扩到 53 项，测试桩补了 section 的建立/删除和非 `wireless` 的
`uci get`。新增覆盖：关闭时逐字不变、开启时的完整映射、幂等、切回关闭后的双向复原、外人设的
`multi_ap=3` 不被误删、回程开放网络/未知加密、以及没有 `access` 字段的旧 plan。53 项全过；
`test_init.sh` 66 项、`test_rpcd.sh` 15 项不变。

> 本地（Windows，`core.autocrlf=true`）直接跑 `test_init.sh` 会失败：工作区文件是 CRLF，而测试桩
> 里的 UCI 解析器不 strip 回车，`config zte_mesh 'main'` 的 `cur` 会带上 `\r`，于是 SSID 读成空。
> 转到 LF 的工作区（或在 Linux 上）就是 66 项。这是 checkout 的问题，不是代码的问题。

#### 仍缺

- **4 地址的真机确认**：UCI 只写了 `multi_ap`；`4addr` 由 hostapd 依这个值去设置。真机上要看的
  是 `ip -d link show` 里回程 BSS 有没有 `4addr on`，以及 `brctl show` 里它在不在 `br-lan`。
- **wpad 变体**：`multi_ap` 需要完整的 wpad（`wpad-mbedtls`），`wpad-basic` 里没有。镜像必须选对。
- **对端**：这一轮只做了 AP 侧（主控方向的下行 BSS）。子路由自己作为 backhaul STA 去关联主控那一半
  （`multi_ap_backhaul_sta`）还没写。


### 11.7 2026-10-29 第六轮：解除主控/子路由的配置耦合

上一轮结束时留下的两个耦合，症状都出在"一个包两种角色"这件事上：配置校验和界面都不知道角色。

**1. 子路由不再需要一份"假 SSID"。** `write_config()` 的档案完整性检查排在 `mesh.role` 被读出来之前，
于是子路由即使 SSID 来自 M2，也必须在本地填一份 2.4G 名称+密码才能启动——用户的第一件事是编一个
永远不会生效的名字。现在先把 `mesh_role` 读出来：角色是 `slave` 且本地档案为空时，从本机
`/etc/config/wireless` 里借一套（`borrow_profile()`），也就是这台子路由**正在广播**的那个网络。
借来的值只用于让守护进程起来，控制器一下发就被 `wlan-sync` 覆盖，所以既没有发明配置，也没有改动射频。
借不到（有线网关，没有 wireless 段）仍然拒绝启动，但错误信息改成说明子路由到底需要什么；
主控角色保持原来的严格行为不变。

为什么不是"干脆允许没有档案"：服务侧的 `parse_config()` 现在要求 `radio0.enabled/ssid/key` 三件齐
（`controller-service.c:516` 那条 `seen != …` 的判定），去掉档案会整份配置被拒、守护进程起不来。
要真做到"子路由无档案"，得同时改解析器和启动时的 `startup_identity()`，代价与风险都不小，而收益只是
省掉一次"借用"。

**2. 界面按角色只显示有效的那一半。** LuCI 里 `mesh_role` 原来只是个下拉框，选 `slave` 之后
"下发给子路由的 SSID / ACL / 漫游阈值"照样全部可见。现在 `render()` 从 `data[2]`（`load()` 里的
`uci.load('zte_mesh')`）读出角色：子路由拿掉"Wi-Fi 配置"和"漫游与转向"两个 tab，也拿掉
`mesh_v2` / `mlo_enable` / 额外网络 / 路由名 / 定时 / MAC 名单几块，只留 `backhaul`；主控侧则
反过来不显示 `backhaul`。表单是渲染期生成的，所以**改角色后要靠 Save & Apply 重新加载页面**，
这与服务重启是同一时刻，不是额外代价。

**3. `status` 带上角色。** rpcd 的 `luci.zte-mesh status` 现在返回 `"role"`（`master|auto|slave`，
未设时按 `master`），脚本不必再读 `/etc/config`。

**4. 应用结果回传。** `wlan-sync` 跑在沙箱外，它做成了什么对服务不可见。现在每次运行都在计划旁边留一份
`wlan-sync.status`（`epoch` / `result` / `changed` / `bands` / `message`，纯 `key=value`，方便 rpcd 直接读），
`status` 把它作为 `"sync"` 对象返回，子路由的状态页多一行"Wi-Fi 同步：已应用 · 12 秒前 · 2g 5g"。
`bands` 记的是**这次真的写了的**频段：`changed()` 同时置 `CHANGED`（要 commit/reload）和 `TOUCHED`
（这一行写了东西），每行开头清 `TOUCHED`，所以一次无变化的运行不会报出上一次的频段——这一点上一版写错过，
测试里"a no-op run names no band"就是为它加的。

**5. 顺手修好的一个测试。** `tests/test_luci.js` 从 `ReportingGridSection` 引入后就没再跑通过
（mock 缺 `GridSection.extend`，接着又缺 `uci.get`），相当于没有覆盖。补齐 mock 之后它暴露了两件事：
`option mode` 是历史遗留项、表单本就不该有；以及新加的角色分流确实生效——现在主控形态注册 31 个主选项，
子路由形态只剩 4 个（`enabled`/`bridge`/`mesh_role`/`backhaul`）。该测试已接进 `make test-host`。

测试：`test_init.sh` 66 → **69**（借用成功、借不到、主控维持拒绝三条），`test_rpcd.sh` 15 → **18**
（角色、`sync` 有值、`sync` 为 null），`test_wlan_sync.sh` 53 → **60**（状态文件六项 + 幂等后不残留频段），
`test_luci.js` 修复后 **通过**。功能总账移到了 [../FEATURE_STATUS.md](../FEATURE_STATUS.md)，那里按难度
排了剩下的项，本文件不再维护清单。

## 12. 2026-10-31 两台设备真的组网了：AL MAC 冲突是唯一堵点

第 11 节把 `mesh_role=slave` 做到"能起来"，但真机上控制器一直 `nodes: []`、两端
`logread | grep -c rx1905` 都是 0。这一节是把它查通的过程与结论。

### 12.1 先给"线上有没有帧"装一只眼睛：`tools/lab/spark.c`

沙箱里的 daemon 到底发没发帧，日志说不清（`sendto` 只被抓到一部分，`send`/`sendmsg` 抓不到）。
`tools/lab/spark.c`（同一个文件，`zig cc -target {x86_64,aarch64}-linux-musl -static` 出两个静态
二进制）只看两件事：往指定接口的 1905.1 组播发一条 Topology Discovery，以及把收到的每个
`0x893a` 帧的源/目的 MAC、CMDU 类型打出来。它自己开混杂模式（不是网桥端口的接口，网卡会先把
组播/单播滤掉，早期"听不到"的结论有一半是这么来的）。

用它直接测出的事实（两端都在同一 L2，路由器只有 `lan2` 有载波）：

| 试验 | 结果 |
|---|---|
| VM 在 `eth0` 发广播，路由器 `br-lan` 收 | 收到（4 帧，60 B，被网卡补零到最小帧长） |
| 路由器从 `lan2` 直接发广播，VM `eth0` 收 | 收不到，但 `tcpdump -i eth0` 能收到 —— 网桥端口的 ingress 帧**不会**再投给绑定在该端口上的 AF_PACKET 套接字 |
| 路由器 `br-lan` 上抓 | 自己的 daemon 每 5 s 一条 `0x0007`（AP-Autoconfiguration Search）+ 每 5 s 一条 `0x0000` |
| VM `br-lan` 上抓 | 静态：一条都没有 |

结论：**子路由一侧的 agent 一直在正常发 Search；控制器一侧一条都不发**。但两边日志都干净，
`actual_lab_bind=1 status=0`、`socket family=17 type=3 protocol=14985 success=1`、
`packet_bind ifindex=5 protocol=35130 status=0` 全都成功。

### 12.2 根因：两端 AL MAC 完全相同

`controller-net.c` 给 `br0` 和 `veth-lab` 分配的 AL MAC 是写死的 `02:00:00:00:19:06`
（`al_last_byte()` 的默认值 06）。`ns-run` 的 service 形态没有覆盖它，于是**两台设备都自称
`02:00:00:00:19:06`**。1905 栈对"源 AL MAC 等于自己"的帧一律丢弃（这是防环的基本规则），
所以两端各自的 Search/Discovery 在自己看来都是"自己发的"，对端发来的又被当成自己的回声：

```
# 修之前，两端抓到的每一帧都是同一个源地址，无法区分是谁发的
02:00:00:00:19:06 > 01:80:c2:00:00:13  cmdu=0x0007  (Search)
02:00:00:00:19:06 > 01:80:c2:00:00:13  cmdu=0x0000  (Discovery)
```

这也解释了为什么第 8、9 节的 lab 能跑通而真机不行：lab 里只有一个节点，自己发自己收，
不存在冲突。

### 12.3 修复：AL MAC 从 LAN MAC 派生，可覆盖

`zte-mesh.init` 在 `start_service()` 里按 LAN 网桥 MAC 的最后一个字节派生出 AL MAC 末字节
（`02:00:00:00:19:XX`，前缀不动，因为 `controller-route.c` 的 `GetRouteIFInfo` 要求它是本地
管理地址），通过 `ZTE_MESH_AL_LAST` 传给 `ns-run`；`zte_mesh.main.al_last` 可以覆盖。

同一个 commit 里补了第二件事：**模拟器的自动探测**。第 11 节为止 `ZTE_MESH_EMULATED_LAB=1` /
`ZTE_MESH_QEMU=/qemu-arm-static` 是手工加在两台设备上的 env，仓库里没有，所以任何人重新部署一次
init 就会把服务打回 `execv errno=8`。现在按 `uname -m` + `/proc/config.gz` 里的 `CONFIG_COMPAT`
判断内核能不能直接跑 ARM32，不能且私有根里有 `qemu-arm-static` 才启用；
`zte_mesh.main.emulator=none` 可以强制走原生路径。

### 12.4 修好之后

AL MAC 变成 `…:03`（VM）与 `…:e3`（路由器，取 `8c:de:f9:b3:d9:e3` 的末字节）之后：

```
01:29:06 02:00:00:00:19:03 > 01:80:c2:00:00:13  cmdu=0x0000  (控制器 Topology Discovery)
01:29:08 02:00:00:00:19:06 > 02:00:00:00:19:03  cmdu=0x0004  (子节点的厂商私有帧，单播回来)
01:29:08 02:00:00:00:19:03 > 02:00:00:00:19:06  cmdu=0x0004  (控制器回应)
```

`ubus call luci.zte-mesh status` 的 `nodes` 第一次出现子节点，而且是 ZTE 身份：

```json
{ "mac": "02:00:00:00:19:e3", "model": "ZXSLC SR1010", "name": "zte-shim-public-lab",
  "zte": true, "online": true, "light": 0 }
```

连上之后稳态流量（各 40 s 采样）：`0x0004` 厂商私有心跳双向各 ~48/s 的对数、
控制器发 `0x0002` Topology Query、子节点回 `0x0006` Link Metric Response（373 B）、
偶发 `0x0000`。控制器同时每 10 s 发一次 `radio_select`，`response_code=1`。

### 12.5 还没到的下一步：M1/M2

`0x0007`（Search）在控制器出现之后就不再发了，而我们一帧 `0x0009`（WSC M1）也没抓到；
子节点侧 `acdCreatWpaApConfileForM1` / `Build M1 phrase` 在启动时确实跑过（每频段一次），
所以 M1 是**生成但没有发出**。也就是说卡点从"两端完全没有 1905"推进到了"枚举（enroll）
这一步没起来"，与第 5 节第 3 条、第 11.5 节的判断一致：agent 的 acdc 状态机需要 cspd 的
cooperator 推一把，而沙箱里没有 cspd。

> 复现：`tools/lab/spark.c` 编出两个静态二进制，放到设备 `/tmp/spark`，
> `spark <iface> listen|discovery [秒]`。注意网桥端口的 ingress 帧对绑定在该端口的套接字不可见，
> 要抓"从线上进来的帧"请用 `tcpdump -i eth0`，要抓"daemon 发了什么"请用 `br-lan`。

## 13. 2026-10-31 第二轮：枚举跑通了，M2 落到子路由的库

第 12.5 节判定的"M1 生成但没发出"是**测量假象**，它后面还藏着三个真 bug。这一节按发现顺序记。

### 13.1 先修测量：协议套接字看不到网桥端口的 ingress 单播

`bind(AF_PACKET, ..., htons(0x893a))` 的套接字挂在协议链表上，投递点在 bridge 的 `rx_handler` **之后**；
`ETH_P_ALL`（`tcpdump` 用的那个）在 `rx_handler` 之**前**分发。所以绑在网桥端口或 `br-lan` 上、按
ethertype 绑定的套接字，看不见从线上进来、被网桥接走的单播帧——`tcpdump` 看得见，自编的工具看不见。

"一帧 `0x0009` 都抓不到"就是这么来的。`tools/lab/enroller.c` 现在用 `ETH_P_ALL` 收帧、自己按 ethertype
过滤，`bind()` 与 `sendto()` 的 `sll_protocol` 也都是 `ETH_P_ALL`。改完第一次跑，M1 就在那儿：

```
tx1905 cmdu=9 bytes=771 sent=771 errno=0     # 2.4G
tx1905 cmdu=9 bytes=801 sent=801 errno=0     # 5G
```

> 方法论：在网桥上探 1905，"收不到"这个结论只有用 `ETH_P_ALL` 复核过才成立。

### 13.2 M1 是被动的：Search 第二段计时器或 `0x0008` 才能触发它

`tools/lab/elfx.py`（自带 ELF32 解析 + capstone；multiapd 没 strip，符号表就够）静态确认：

| 入口 | 事件号 | 行为 |
|---|---|---|
| `_acdcSearchHandler` | 2（Search 第二段计时器到期） | 调 `_acdcSendApAutoConfigWSCM1` |
| `_acdcConfigHandler` | 8（收到 `0x0008` AP-Autoconfiguration Response） | 调 `_acdcSendApAutoConfigWSCM1` |

而控制器自己发 Search 是**被门控**的：`acdsTimerMsg` 里 `[acdGetBasicInfo() + 0x114 + radio*12]` 为 0
就直接跳过，这个标志只有 cspd 在配置完 AP 之后才置位（沙箱里没有 cspd）。于是两头都在等对方：
agent 等 Search/Response，控制器等 cspd，`0x0007` 在控制器出现之后一条都不发。

### 13.3 补上控制器缺的那一步：`tools/lab/enroller.c`

一个静态二进制，监听子节点的 `0x0007`，用 `0x0008` 回它；`--al-mac` 可以冒充控制器的 AL MAC，让 M1
单播回沙箱控制器，由它跑真正的 M2。帧形状与 `controller-packet.c:controller_autoconfig_response()`
逐字节一致（35 B：`0x0f` searched role=registrar、`0x10` frequency band、`0x80` service=controller）。

线上时序（`tools/lab/enroller.c` + `libzte_controller_trace.so` 双端）：

```
agent   → 01:80:c2:00:00:13   0x0007  Search        240 B   TLV 0x0e band / 0x80 0x81 service / 0xb3 profile
enroller→ agent               0x0008  Response       35 B
agent   → enroller            0x0009  WSC M1        771 B（2.4G）/ 801 B（5G）
controller → agent            0x0009  WSC M2        911 B   TLV 0x11 len=656，内含 0x1022=5；另 0x82 "MAP24G"、0xe0、0x0b
agent   → controller          0x0009  WSC           218 B
```

### 13.4 第二个 bug：运行包从来没打包 `libzte_agent_mm.so`

M2 到了，`acdcConfigM2EncryptedSettings()` 却每次都在第一行就 `-3` 中断：

```
0x75e18  cmn r0,#9          ; 返回非 0 且非 -9 一律当致命错误
         beq  ...
```

第 11.1 节做的替身一直在 `zte-mesh.init` 的 `LD_PRELOAD` 里，但 `zte-mesh-runtime.tar.gz` 里没有这个库
（`tar -tzf` 对照 Makefile：`libzte_controller_profile/route/trace` + `libzte_mesh_shim` 都在，就缺它）。
所以"库里写了、包里没有"——任何人装一遍包，enroll 都会回到第 8 节之前的样子。

修：CI 加 `libzte_agent_mm.so` 的构建（原厂 glibc、`-nostdlib` 链接，`nm -D --defined-only` 记录导出符号，
只有 `CmWLANSetAPConfig` / `CmWLANSetPsk` / `zte_agent_mm_layout`），再用 `openwrt/repack-runtime.py`
重打运行包：逐成员逐字节不变，只替换/新增一个库并重写 `SHA256SUMS`。

```
old members 48  new members 49
changed bytes: ['root/lib/libzte_mesh_shim.so', 'SHA256SUMS']
added: ['root/lib/libzte_agent_mm.so']   removed: []   mode diffs: []
sha256 mismatches: []  entries: 38
```

装上之后（不再需要任何 flag 文件）：

```
ZTE_AGENT_MM setter=CmWLANSetAPConfig instid=DEV.WIFI.AP1 status=-3 row_applied=1 report=0 errno=9
ZTE_AGENT_MM setter=CmWLANSetAPConfig instid=DEV.WIFI.AP5 status=-3 row_applied=1 report=0 errno=9
```

`row_applied=1` 是"库里已经是调用方要的值"。可见证据：子节点 `/run/zte-mesh/wlan-plan` 里
`0.ssid` / `1.ssid` 从 `ImmortalWrt` 变成控制器上改的 `ZTEMESHCTRL`。`CmWLANSetPsk` 返回 `-9`
（`row_applied=0`）是 `acdcConfigM2EncryptedSettings` 的"已配置，跳过"语义，属预期，不是失败。

### 13.5 第三个 bug：`wlan-sync` 的 `stamp()` 在没 `stat` 的镜像上恒为 `missing`

`stamp()` 原来用 `stat -c '%Y:%s'` 做变更检测，而该镜像没有 `stat`（`which stat` 空），`|| echo missing`
把 stamp 变成常量：主循环 `if [ "$current" != "$last" ]` 只成立一次，于是**启动那一遍之后再也不会应用
plan**。对一个被 M2 现场配置的子路由来说这正好是反的：plan 是入网之后才出现的，而没人去应用它。

改成 content 摘要（`md5sum` 优先，`stat` 次之，`ls -ln` 兜底）。之后日志出现
`applied; wireless committed and reloaded`，`uci show wireless` 里 `ssid='ZTEMESHCTRL'` 落地。

### 13.6 第四个 bug：band 是名字，不是 section

`radio_section_of 5g` 原来取"第一个 band 为 5g 的 wifi-device"。实验室这台 ax3600 有**两个**：
`radio0`（PCIe 5G 卡，ath10k，`disabled=1`）与 `radio1`（SoC 的 5G，ath11k）。于是控制器下发的 5G 行
写进了那张空卡的 `default_radio0`，真正在空中的 `default_radio1` 一直是 `ImmortalWrt`——看起来就像
"5G 行没被应用"。

现在按顺序判定：`zte_mesh.main.wifi_2g` / `wifi_5g` 明确指定 > 该 band 只有一个设备 > 该 band 只有一个
设备没被关掉（老镜像常把备用 radio 关着）。两个都活着且没有显式指定时**不猜**：该 band 一个字段都不写，
日志里点名 `zte_mesh.main.wifi_5g`。写错设备比不写更难查——空卡被写上控制器的 SSID，而空中什么都没有。

### 13.7 现在的事实

| 项 | 值 |
|---|---|
| 控制器（VM 192.168.1.20） | `running=1`，`nodes` 含 `02:00:00:00:19:e3`（`zte=true online=true`） |
| 子路由（192.168.1.1） | `running=1`，`cur_hop=2 max_hop=2`（此前 `-2`） |
| M1 次数 | 一个服务生命周期 1 次（此前 3 次） |
| 下发的 Wi-Fi | `/run/zte-mesh/wlan-plan` 与 `/etc/config/wireless` 都是控制器的 `ZTEMESHCTRL` |
| 未落地 | 本机三个 `wifi-device` 都是 `disabled=1`（镜像/自配状态），所以 BSS 谁都没起来；见 13.8，现在由 `wlan-sync` 在 plan 启用某 band 时把对应设备打开 |

### 13.8 第五个改动（B 方案）：plan 说这个 band 要开，就把关着的 radio 设备打开

配置已经对了、空中什么都没有，是因为 radio 设备本身 `disabled=1`——而 `wlan-sync` 过去只写 `wifi-iface`，
从不碰 radio 的开关。这在这台机上不是偶发：出厂镜像和实验室的 ax3600，三个 `wifi-device` **全是关的**。

现在 `apply_plan` 在某个 band 的主 BSS `enabled=1` 时调 `enable_radio`；backhaul 行也一样，回程 BSS 同样需要
radio 开着才有地方承载。

- 只写 `disabled=0`，从不写 1。plan 说这个 band 关的时候一个字段都不碰设备：把设备关掉会连带把该 band 的
  backhaul BSS 一起杀掉，而那是控制面的通路。
- 不是看门狗：它只在 plan 变化时跑一次。用户之后手动关掉的 radio 会一直关着，直到控制器下发不同的 profile。
- UCI 布尔有词形（`true` / `on` / `yes`），手改的配置会用；`radio_off()` 一并认，`radio_section_of` 判断
  "哪个候选还活着"也改用它，免得两个函数对"关着"的理解不一致。

### 13.9 第六个 bug：`E([ ..., null, ... ])` 让子路由角色整页空白

选成子路由之后，页面抛 `TypeError: Cannot read properties of null (reading 'charCodeAt')`。起因在 `render()`
的收尾：

```js
return E([
    E('div', ...),                 // Status
    child ? null : E('div', ...),  // Child routers
    formNode
]);
```

`dom.create()` 对数组是**逐个元素**递归调 `create()`，而 `create()` 的第一件事是 `html.charCodeAt(0)`，
于是 `create(null)` 直接抛。`E('div', attrs, [ok, null])` 不会抛：那条路走的是 `append()`，它跳过 null。
差别就在"数组元素"与"子节点"。

修法是那一节**不进列表**（`sections.push`），不是往列表里塞 `null`。

测试为什么没抓到：`tests/test_luci.js` 的 `E` 桩接受任何参数，连 `null` 都包成一个对象。现在桩按
`dom.create()` 的语义实现（数组递归展开、首参为 null 时抛同一个 TypeError），改视图前跑一次就能复现同一个
异常，改后通过——子路由页面至少不会因为这条路径再白一次。


## 14. 2026-10-31 第三轮：角色拆成三个，主路由也能把档案落到本机

### 14.1 为什么拆

原厂那台"主路由"是**既当主控、又自己发 Wi-Fi**的盒子，一个开关就够。但 OpenWrt 这边的现实是反过来的：
SR1010 这类机器是**有线网关、没有射频**，当年给它做主的正是它。所以"控制"和"自己也发"必须能分开：

| `mesh_role` | 给子路由下发档案 | 应用到本机 `/etc/config/wireless` | 线上 `mesh.role` |
|---|---|---|---|
| `master`（默认；未设置也等于它） | 是 | **不动** | `master` |
| `main` | 是 | **是**（plan 由 `write_config` 写） | `auto` |
| `slave` | 否 | 是（plan 由服务从 M2 读出来写） | `slave` |

`main` 用的就是原厂的"主路由 / 自适应"，所以**线上一个字节没改**：还是 `auto`，还是 `Mode 0 → 2` 的选举替身
（第 12 节）。旧的 `auto` 值继续被接受、含义与 `main` 相同，升级过的配置不用动。反过来，语义变化只有一处：
以前 LuCI 把 `master` 叫作"主路由"，现在 `master` 是"仅主控"——它在**行为上**和以前一样（以前也没人改本机
无线），只是名字改了；想真的把网络发出去，得选"主路由"。

### 14.2 一个 plan 只能有一个生产者

子路由的 plan 是 `agent-sync.c` 从 `WLANCfg`/`WLANPSK` 读出来的（第 11 节），主路由没有 M2 可等——它就是
发 M2 的那台。所以主路由的 plan 由 `write_config` 直接写（`write_local_plan`），格式与 `agent-sync.c`
逐字段相同（`rows=`、`i.radio/slot/ssid/key/enabled/hidden/access/beacon_type`），`wlan-sync` 不关心来源。

三条规则，写在 `write_config` 的 case 里：

- `main`/`auto`：写 `$RUNDIR/wlan-plan`（0600）。
- `slave`：**一个字节都不碰**。`write_config` 在 reload 时也会跑，删掉它等于把正在用的 plan 抽走，而服务只在
  行变化时才重发（`agent_sync_reset()` 只在启动时调）——子路由会安静地停止应用。`tests/test_init.sh` 的
  `role-slave-keeps-plan` 就是钉这个的。
- 其余（`master`、空）：`rm -f`。上一步角色留下的 plan 不该被一个已经不跑的 `wlan-sync` 应用。

### 14.3 5 GHz 的派生值要写第二遍，这是这次改动里唯一的知识重复

daemon 自己会用 `wifi.dual_band_sync` / `wifi.essid_suffix` 派生 5G 的名称和密钥，而 plan 必须**写全**。
第二遍推导就是漂移的来源，所以：`emit_band_options` 顺手把该频段的 `hidden` / `beacon_type` 读回给调用方
（`BAND_HIDDEN` / `BAND_BEACON`），5G 的名称/密钥只在 `write_config` 里算一次（`s1_air` / `k1_air`），
emit 与 plan 都用它。测试用 `role-main`（sync 关、5G 全空 → `home-5G` / 2.4G 的密钥）和 `role-main-sync`
（sync 开、`wifi_5g_ssid` 写了也被忽略、隐藏与加密跟随 2.4G）两条用例把它钉住。

### 14.4 设备证据

控制器（VM，192.168.1.20，**零个 wifi-device**）`mesh_role=main` 之后：

```
/run/zte-mesh/wlan-plan   316 B, -rw-------, rows=2
  0.ssid=ZTEMESHCTRL  0.key=Ztemesh2631!  0.hidden=0  0.beacon_type=12  0.access=32
  1.ssid=ZTEMESHCTRL  1.key=Ztemesh2631!  1.hidden=0  1.beacon_type=12  1.access=32
ZTE_MESH_SERVICE cfg mode=2 set_role_enable=1 en_master_only=1 cur_hop=2 max_hop=2
wlan-sync: no radio device for 2g; left alone
wlan-sync: no radio device for 5g; left alone
wlan-sync: plan applied nothing new          # status: result=ok changed=0
ubus call luci.zte-mesh status → "role": "main"
```

`procd` 里多了一个 `wlan-sync` 实例；`mesh_role=master` 之后同一个文件消失、实例消失、`"role": "master"`，
子节点（`02:00:00:00:19:e3`）全程 `online=1`。

子路由（ax3600，三个 wifi-device）上用临时 `RUNDIR` 直接调 `write_config`（不动正在跑的服务）：

```
rc=0   /tmp/r2/wlan-plan 316 B -rw-------
0.ssid=ImmortalWrt  0.key=Ztemesh2631!      # 本机自己的档案（sync=1，5G 跟随 2.4G）
```

也就是说：**没有射频的控制器**里这台逻辑一条不差地跑通（只是最后一步无事可做），**有射频的机器**上它产出
的 plan 就是那台机器该广播的东西。

### 14.5 有意的取舍

没有加 `apply_local_wifi` 之类的开关：**角色本身就是那个开关**。多一个布尔值就多一种"角色说主路由、开关说
不要"的组合，而这两种状态在页面上没法解释。仅主控要是还有射频，启动日志里会明说一句
（`controller-only role: the Wi-Fi profile goes to the child routers only ...`），因为这是最容易看成"plan
没生效"的一种配置。
