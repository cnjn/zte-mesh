# sr1010-mesh：SR1010 上用原厂 multiapd 当 mesh 主控（AC）

SR1010 刷 OpenWrt/ImmortalWrt（有线网关，没有 Wi-Fi）以后，继续管理保留原厂固件的中兴子路由（测试机 ZXHN Z506）。做法是把原厂
ARM32 `multiapd` 当控制器跑在私有沙箱里，周围用数据库行、两个 CM 函数的替身和一个服务配置文件指挥它，**不重写 mesh 协议**。

## 三种角色（`zte_mesh.main.mesh_role`）

| 角色 | 作用 |
|---|---|
| `master`（默认） | 只当主控：把 Wi-Fi 档案下发给子路由，**本机无线完全不动**。SR1010 这类有线网关就是这种 |
| `main` | 主控 + 自己发 Wi-Fi：同一份档案既下发，也落到本机 `/etc/config/wireless`。即原厂的"主路由 / 自适应"，线上仍发 `mesh.role=auto`，旧值 `auto` 同义 |
| `slave` | 本机当子节点：收 M2 并把名称/密码/加密写到本机；`backhaul=multi_ap` 时把隐藏回程落成 4 地址 BSS。需要完整 `wpad`，真机数据还没有（[AGENT_PROBE](docs/research/AGENT_PROBE.md) 第 11 节） |

## 兼容范围

设计上不绑定型号：主控可以是任意刷 OpenWrt/ImmortalWrt 的设备，子节点可以是中兴原厂固件的路由器，也可以是另一台 OpenWrt 路由器。
但**实际打包出来的只有 aarch64 目标**（由 `zte-mesh-launcher` 的 `PKGARCH` 与 `DEPENDS` 决定），其他架构要自己把 `qemu-arm-static`
放进私有根，让原厂 32 位 ARM 程序跑起来（见下面"启用前"那段与 [DEVELOPMENT](docs/DEVELOPMENT.md) 第 11 节）。

端到端组网目前**只用非中兴设备做过**：主控是 x86_64 虚拟机，子节点是小米 AX3600 刷 ImmortalWrt（有线回程）。中兴 Z506 只做过
主控侧联调（入网、SSID/密码、多 BSS、BeaconInterval、ACL、节点动作等），没有用它跑过 `slave` 角色。

## 当前状态

**功能总账只有一份**：[docs/FEATURE_STATUS.md](docs/FEATURE_STATUS.md)（逐项状态与证据、做不到的项、按难度排的推进顺序）。
这里只留三条结论：

- 主控侧在真子路由 Z506（原厂固件）上验证通过：入网、SSID/密码/多 BSS、BeaconInterval、ACL、信道与功率、节点动作、定时窗口。
- 子路由侧（本机当子节点）在两台实验机上打通有线回程，主控改 SSID 后子路由的空中名称跟着变。
- 打包完成：`zte-mesh`（`arch: all`）、`zte-mesh-launcher`（aarch64，现编 `ns-run`/`mesh-search`）、`luci-app-zte-mesh`、
  `luci-i18n-zte-mesh-zh-cn` 四个 apk，由 `build-apk.yml` **手动**触发构建。

启用前先确认一件事：原厂 `multiapd` 是 **32 位 ARM** 程序，设备内核跑不了它就需要 `qemu-arm-static`，而这个模拟器**不在任何包里**，
要手工放进私有根；arm64 设备刷 `CONFIG_COMPAT=y` 的固件即可原生运行。判定逻辑、三种出路和报错含义见
[DEVELOPMENT](docs/DEVELOPMENT.md) 第 11.2 节。

没做或没验证：用已安装的包接真实子节点；LuCI 页面在浏览器里的效果；长时间稳定性；多个子节点；关主 SSID 的路径
（`radioN.enabled=0`、定时 `scope=all`），后者会断开用户自己的 Wi-Fi。

## 目录

| 路径 | 内容 |
|---|---|
| 根目录 `*.c` `*.h` `*.def` `*.inc` | 沙箱启动器、服务、垫片和 preload 库、命令行工具（见 [ARCHITECTURE](docs/ARCHITECTURE.md)） |
| 根目录 `*.py`、`build.sh` | 运行环境提取与审计、证据核对、实验室/设备测试脚本 |
| `tests/` | 主机测试（C 编解码、Python、init/rpcd 的 shell 测试、LuCI 的 JS 测试） |
| `openwrt/` | OpenWrt 包：`zte-mesh`、`zte-mesh-launcher`、`luci-app-zte-mesh`，以及 `make-runtime.py` |
| `tools/build/`、`tools/capture/`、`tools/child/`、`tools/probe/` | 构建容器与打包、抓包重放、子路由 telnet、静态分析 |
| `docs/` | 文档（索引见下） |
| `out/`、`evidence/` | 构建产物与实验证据，含原厂二进制，**不入库** |

原厂固件镜像不在这里：`prepare.py` 默认读 `../zx279133/vendor-reference/2b5/rootfs.jffs2`，可用 `SR1010_VENDOR_ROOTFS` 指定。

## 快速开始（在 mac 上）

```sh
orbctl start                                   # Docker（OrbStack）默认不运行
sh build.sh                                    # 提取运行环境、审计、编译、主机测试、离线加载测试
sh tools/build/container-make.sh test-host     # 只跑主机测试；改某个产物：container-make.sh db-engine-probe
sh tools/build/runtime.sh svctest              # 实验室 tmpfs 运行包 out/zte-mesh-svctest.tar.gz
python3 dev-service-smoke.py --no-attach --seconds 24   # 路由器上完全私有地跑一次服务（不接 LAN）
sh tools/build/runtime.sh install              # 包用的运行包 openwrt/zte-mesh/zte-mesh-runtime.tar.gz（随仓库提交）
```

包的构建、真子节点测试的规矩和命令、分支与产物托管见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE](docs/ARCHITECTURE.md) | 原理、运行时结构、源文件职责、固件来源与哈希、ABI 与沙箱约束、为什么选这条路线 |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | 机器与连接、构建、打包、跑控制器、测试、真机守则、踩过的坑、ARM32 宿主件、**ARM32 模拟器不入包的原因与手工放置** |
| [SERVICE](docs/SERVICE.md) | 服务模式：配置文件、热更新、守护进程重启、节点动作、UCI/LuCI 映射 |
| [REAL_CHILD](docs/REAL_CHILD.md) | 真子路由联调：入网门槛、**服务配置语法**、各功能验证记录、风险 |
| [FINDINGS](docs/FINDINGS.md) | 逆向与真机发现：角色选举、自组网 V2、`VersionMode`、`CurHop`、ACL 等 |
| [CONTROLLER_CAPABILITIES](docs/CONTROLLER_CAPABILITIES.md) | 原厂控制器能下发什么、Z506 实际支持什么、本项目覆盖了什么 |
| [FEATURE_STATUS](docs/FEATURE_STATUS.md) | **功能总账**：状态与证据、做不到的项、推进顺序 |
| [research/](docs/research/) | 原厂对照研究（存档）：`FEATURE_MAP`、`GAP_ANALYSIS`、`REPLAY_RESULTS`、`AGENT_PROBE`、`E2633_FIRMWARE` |
| [milestones/](docs/milestones/) | 早期里程碑 B.1–B.3.3 的验收记录（存档） |
| [tools/README.md](tools/README.md) | 各个脚本 |

`docs/research/` 与 `docs/milestones/` 是**证据存档**，按时间保留原始记录、不随结论更新；结论以上面八份为准。

## 约定

- 仓库里不放任何密码和密钥。子路由 telnet 口令只从环境变量 `ZTE_TN_PASS` 读；服务配置里的 Wi-Fi 密钥只写进 0600 临时文件，用完删除。
- `evidence/` 和 `out/` 里有原厂二进制、WSC 材料和原厂整库，不入库，不外传。
- 原厂程序和库带旧 OpenSSL，产物只用于私有工程验证；对外分发前要另外处理安全维护、最小权限和二进制再分发许可。
