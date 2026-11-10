# luci-app-zte-mesh：让 OpenWrt 路由器与中兴路由器组网

给 OpenWrt / ImmortalWrt 做的中兴组网插件。OpenWrt 侧既能当**主控**（把 Wi-Fi 配置下发给子路由），也能当**子节点**
（加入网络，配置由主控下发）；**中兴侧保留原厂固件，不刷机**。做法是把原厂 ARM32 `multiapd` 当控制器跑在私有沙箱里，
周围用数据库行、两个 CM 函数替身和一个服务配置文件指挥它——**不重写 mesh 协议**，所以线上帧与原厂一致。

参考实现机：ZXSLC SR1010（又叫中兴星云 MAX）刷 OpenWrt/ImmortalWrt，是有线网关、不带 Wi-Fi。中兴原厂路由器：ZXHN Z506（又叫中兴小方糖），保留原厂固件不刷机。

## 功能表

状态只有三种：**✅ 已实现并验证** / **◐ 已实现，还没在真实路由器上验** / **⬜ 没做**。逐项状态与证据在
[docs/FEATURE_STATUS.md](docs/FEATURE_STATUS.md)，等上机的项在 [docs/PLAN.md](docs/PLAN.md) 第 5 节。

### 本机当主控（给中兴原厂路由器下发 Wi-Fi）

| 功能 | |
|---|---|
| 网页上配主 SSID：名称 / 密码 / 加密 / 是否隐藏 | ✅ |
| 自动发现新子路由并接纳（约 5 分钟一轮，不用手工配对） | ✅ |
| 多台子路由同时在线 | ◐ |
| 改完配置热更新下发，不重启服务 | ✅ |
| 多网络（访客 / 备用 / 隐藏回程，网络序号 1–3）的名称、密码、开关 | ✅ |
| 信道与发射功率 | ✅ |
| 终端黑白名单，可按每个网络单独设"关 / 拦 / 只放行" | ✅ |
| BeaconInterval | ✅ |
| 访客限时、Wi-Fi 定时关闭 | ✅ |
| 远程重启子路由、开关指示灯 | ✅ |
| 「MESH 兼容」开关（开 = 接受任何子路由，关 = 只收市场码相同的中兴路由） | ✅ |
| 拓扑页：节点清单（型号、名称、地址、MAC、在线状态） | ✅ |
| 拓扑页：直连还是经中继、BSS 数、上报新鲜度、每节点终端数（按 BSSID 分开统计，按 BSSID 一列）、固件版本 | ◐ |
| 拓扑页：回程 RSSI、树状图、每 AP 的终端列表 | ⬜ |
| 引导漫游（BTM）/ 封禁终端 / 踢终端下线 | ◐ |
| 漫游与频段引导阈值、MLO 开关、Wi-Fi 5 兼容标志下发 | ◐ |
| 本机也发 Wi-Fi（原厂意义上的"主路由"） | ◐ |

### 本机当子节点（加入网络）

| 功能 | |
|---|---|
| 网线插主控 LAN 口就入网，本机不用填任何 Wi-Fi 参数 | ✅ |
| 收主控下发的名称 / 密码 / 加密并写进本机无线配置，只在确实有改动时才写文件 | ✅ |
| 入网时先沿用本机正在广播的名字，重启服务**不再把本机广播的 SSID 改两次**（不掉线） | ✅ |
| 应用结果回传页面（成功 / 失败 / 改了哪些频段） | ◐ |
| 额外网络（访客等）按序号写到本机射频 | ◐ |
| 接另一台 OpenWrt 路由器当主控（有线回程） | ✅ |
| 接中兴原厂路由器当主控（本机当子节点） | ◐ |
| 无线回程（4 地址回程 STA）：配置面已生成，端到端没有真实路由器上的数据 | ◐ |

### 页面、安装与运行环境

| 功能 | |
|---|---|
| LuCI 页面，中英双语（中文随 CI 出 `zh-cn.lmo`） | ✅ |
| 两个开关组合出三种拓扑，页面按角色只显示有效项 | ✅ |
| 三个 apk：`zte-mesh`（运行环境 + 启动器）/ `luci-app-zte-mesh` / `luci-i18n-zte-mesh-zh-cn` | ✅ |
| 启动器不挑机器：一个 apk 带 aarch64 / armv7 / x86_64 各一份构建，装机时不再有架构判定，"这台不在包里"由启动器在运行时说清（三种架构分别怎么编、字节数、有没有在真实路由器上读过见下面的「支持的架构」） | ◐（x86_64 与 aarch64 两种架构已在两台测试环境读到，含对照组那次改名；armv7 那种架构没有机器，只有编译与打包级的证据，见 [PLAN](docs/PLAN.md) H-16） |
| 一条命令装机验收（装包 + 读回版本 + 四条冒烟 + 读回这批的标记） | ✅ |
| 卸载与重装（`apk del` / `apk add` 循环走通） | ✅ |
| 作为 ImmortalWrt 的 feed 登记，三个包在那棵树里编出来（CI 出的 apk 就是这么来的） | ✅ |
| 预装进固件镜像、刷到设备 | ⬜ |
| arm64 设备直接跑 32 位原厂程序：内核能跑就原生跑（探测已修，ax3600 实测） | ✅ |
| 内核跑不了 ARM32 时用 `qemu-arm-static`：**支持手工放，不随包分发**（有意的取舍）；放的位置可以是这台机器上的任意路径，`zte_mesh.main.emulator` 指过去即可（怎么判断这台需不需要、放哪、四个坑见下面的「qemu-arm-static 安装指南」） | ◐ |
| 两个频段广播不同名字时各按各自的写（不派生） | ◐ |

## 支持的架构

两件事的限制不在同一层：**启动器**（我们自己编的那两个二进制，跟这台机器的 CPU 走）和**原厂控制器**
（32 位 ARM 的 `multiapd`，跟这台机器的内核能不能执行 ARM32 走）。`zte-mesh` 这个包是 `arch: all`，里面按 CPU
各放一份启动器构建，运行时由 `uname -m` 决定进哪个目录，所以设备的 profile 串（`/etc/apk/arch` 里那行
`aarch64_cortex-a53`）不再挡装机。

| 这台机器的 CPU（`uname -m`） | 启动器用哪种架构 | 谁编出来的 | 这一轮的字节数（`ns-run` / `mesh-search`） | 真实机器读到过吗 |
|---|---|---|---|---|
| `aarch64`（`arm64`） | `arch/aarch64/` | ImmortalWrt SDK 自己的 musl 工具链 | 61 703 / 66 039 | ✅ 小米 AX3600，当子节点 |
| `x86_64` | `arch/x86_64/` | 构建机上的 `musl-gcc`。**编不出来才退回宿主 `cc`**，退回来那对是 735 932 / 854 716（把整份 glibc 静态搬进去），CI 判红 | 61 719 / 61 975 | ✅ ImmortalWrt 虚拟机，当主控 |
| `armv7`（`armv7l`） | `arch/armv7/` | Ubuntu 的 `arm-linux-gnueabihf-gcc`。这种架构没有 musl 的交叉编译器，所以字节数只报数、不设上限 | 356 373 / 389 117 | ⬜ 三台测试环境都不是这种架构；ZXHN Z506 吃原厂固件、不装 apk |
| 其他（mips、riscv…） | 不在包里 | — | — | 包照样装得上，启动器运行时说清"这台没带这种架构的构建"，不起服务 |

前三行的字节数是 `ci/batch24`（包版本串 `0.1.0-r23`）那一轮 CI 日志里的六行读数，与两台机器上 `wc -c` 逐字对上，
也和 `ci/batch23` 那六行一字未变；要 musl 的那两种架构每份上限 204 800 字节。armv7 那种架构缺运行时证据，记在
[PLAN](docs/PLAN.md) H-16。

| 这台机器的内核 | 原厂 ARM32 程序怎么跑 |
|---|---|
| 32 位 ARM（`armv7l`） | 原生执行，不需要模拟器；但 init 的判定表里只有 aarch64 那一个分支会去问内核，`armv7l` 会先打一条"这个内核跑不了 32 位 ARM 守护进程"再照常原生起——服务是对的，那句话在这类机器上是错的，已登记 [PLAN](docs/PLAN.md) 第 3 节「25 待排」 |
| aarch64 / arm64 | 内核开了 `CONFIG_COMPAT=y` 就能原生执行；读不到配置就直接试一次 `lib/ld-linux.so.3`。跑不了就得手工放 `qemu-arm-static` |
| x86_64 及其他 | 一定要 `qemu-arm-static` |

`qemu-arm-static` **不随包分发**（有意的取舍），放哪都行，`zte_mesh.main.emulator` 指过去即可。
装它的完整步骤在下面的「qemu-arm-static 安装指南」，判定细节与三条出路的原始记录在
[DEVELOPMENT](docs/DEVELOPMENT.md) 第 11.2 节。

## 测试情况

三层各答一件事：**主机测试**答"这一批代码对不对"，**CI**答"这三个包是不是这一批、每份二进制是不是这台机器能跑的
那种架构"，**装机验收**答"这台机器上真跑起来了没有"。逐条判据在 [FEATURE_STATUS](docs/FEATURE_STATUS.md)，
条数基线与各种桩的坑在 [DEVELOPMENT](docs/DEVELOPMENT.md) 第 4 节。

### 主机测试（不用上机器）

```sh
sh tools/build/container-make.sh test-host     # 全套，要在构建容器里
sh tests/test_init.sh                          # 下面这些组本机器就能单独跑
METRICS_READS=1 METRICS_READ_GAP=0 SSID_WAIT=0 sh tests/test_verify_install.sh
python tests/test_i18n.py                      # 改了界面文案必须跑
```

| 组 | 条数 | 它守什么 | 本机（Windows + Git Bash）跑得起来吗 |
|---|---|---|---|
| `tests/test_init.sh` | 158 | 真的**执行** init 里的 `write_config()` 与预检 `launcher_ready()`——启动前那四条检查各说各的话 | ✅ |
| `tests/test_verify_install.sh` | 278 | 装机验收脚本每条判定，含它"问得太早就不作判定"的那些分支（uci/iw/ip/date 全是桩） | ✅，但要导出 `METRICS_READS=1`、`METRICS_READ_GAP=0`、`SSID_WAIT=0`，否则每个"这台没有数"的用例真睡满 10 秒 |
| `tests/test_wlan_sync.sh` | 182 | 主控下发的配置写进本机无线配置：实际改了哪些频段、额外网络按网络序号、回程 STA 段 | ✅ |
| `tests/test_sta_collect.sh` | 32 | 子节点的终端数采集、`/run/zte-mesh/ap-sta` 那张表和那个暂停开关 | ✅ |
| `tests/test_launcher_arches.sh` | 26 | `launcher-arches.sh` 的编译器选址与 `ZTE_MESH_REQUIRE_ARCHES`（编译器是桩，判的不是真编译） | ✅ |
| `tests/test_launcher_dispatch.sh` | 20（另 skip 2） | 那份按 `$0` 认程序、按 `uname -m` 选目录的入口脚本 | ⚠️ skip 2 条：NTFS 上 `chmod 644` 清不掉执行位，"文件在但不可执行"这一支在本机表达不出来 |
| `tests/test_runtime.py` | 30 | 随包那个 tarball 的成员与仓库文件逐字节一致——防"包里那份是旧的" | ✅ |
| `tests/test_i18n.py` | 12 | 页面文案与 `.po` 对得上 | ✅ |
| `tests/test_luci.js` | 38 个主选项 + 10 个网络序号 + 4 条 ACL + 2 个客户端项 + 子表单 7 项 | 页面表单结构（自己模拟 LuCI 的 `form`/`uci`/`rpc`，不要 LuCI 运行时） | ⚠️ 能跑，但 PATH 里没有 `node`，得给那个独立 `node.exe` 的绝对路径 |
| `tests/test_rpcd.sh` | 24 | rpcd 插件的状态上报与下发接口 | ⚠️ 它的 `jsonfilter` 桩写死调 `python3`，本机只有 `python`——给一个转发桩才 24 条全绿 |
| C 编解码（含 10 万次变异）、preload、沙箱、`cspd` 证据那四组 | — | 线上帧的编解码与原厂库的表结构 | ❌ 在构建容器里跑；`cspd` 那几组还要原厂镜像（`evidence/` 不入库） |

上面那一列条数是 2026-11-06 在本机实测的读数：init 158／装机验收 278／wlan-sync 182／终端数采集 32／架构构建 26／
入口 20（skip 2）／runtime 30／i18n 12／LuCI JS 全绿，`0 failed`。

### CI（手动触发，push 不会跑）

`build-apk.yml` 一轮约 20–25 分钟，判据不是"编译通过"：三个 apk 都要在；**用 SDK 自己那份 `apk` 解出 `zte-mesh`
的成员**，确认 `arch/{aarch64,armv7,x86_64}/{ns-run,mesh-search}` 和两个外层入口都在；`file -b` 问每个二进制
"你是不是路径上写的那种架构"（只看路径会放过"同一份构建复制三遍"那种装法）；要 musl 的那两种架构每份上限
204 800 字节；六行重量同时写进 job log；`ZTE_MESH_REQUIRE_ARCHES` 少编一种架构直接红。

最近一轮是 2026-11-06 的 `ci/batch24`（run `42209996117`，**19 分 44 秒**），产出三个文件名都带 `0.1.0-r23` 的 apk
和一份 `SHA256SUMS.txt`；构建计时的两段读数在 [DEVELOPMENT](docs/DEVELOPMENT.md) 第 8 节。

### 装机验收（在设备上）

```sh
sh tools/dev/verify-install.sh /tmp/zte-apk    # 真装包、真重启服务
sh tools/dev/verify-install.sh                 # 只读不装
```

它打印一串 `KEY=VALUE` 证据行和最后一行 `RESULT=PASS|FAIL`：版本串、三条启动器断言、这批的包内脚本标记、
终端数那张表、这一趟重启写了几次无线配置。每个键答什么、哪些情况**明确不作判定**，写在
[INSTALL](docs/INSTALL.md) 第 3 节那张表里——判定逻辑本身由上面那 278 条主机断言守着。

`0.1.0-r23` 这一轮两台都过：ImmortalWrt 虚拟机（当主控）`pass=18 fail=0`，小米 AX3600（当子节点）`pass=19 fail=0`，
两边的 `apk list` 都读出三个包同为 `0.1.0-r23`，启动器分别走到 `arch/x86_64/ns-run` 与 `arch/aarch64/ns-run`。

### 还没读到的（不写成结论）

1. **armv7 这种架构没有机器**：只有编译与打包级证据，运行时读数欠着（[PLAN](docs/PLAN.md) H-16）。
2. **ZXHN Z506（中兴原厂路由器）这几版没参与**：子节点侧真实终端数上报没在这台机器上读到过。
3. **页面「终端数」按 BSSID 那一列**没在浏览器里看过有内容的行——数据源在真实路由器上是活的。

## 设备角色（两个开关）

| 组合 | 作用 |
|---|---|
| `mesh_role=master` + `apply_local=0`（默认） | 只当主控，把配置下发给子路由，**本机无线一个字节都不动** |
| `mesh_role=master` + `apply_local=1` | 主控且自己发 Wi-Fi（原厂页面里的"主路由"）；旧值 `main`/`auto` 仍被读，等于这一组合 |
| `mesh_role=slave` | 本机当子节点，配置由主控下发；`backhaul=multi_ap` 时回程写成客户端段，需要完整 `wpad` |

## 上手

三个 apk（`zte-mesh` / `luci-app-zte-mesh` / `luci-i18n-zte-mesh-zh-cn`）与 `SHA256SUMS.txt` 已发在
[GitHub Release `v0.1.0-r23`](https://github.com/Leenshady/luci-app-zte-mesh/releases/tag/v0.1.0-r23)，
四个文件都能匿名下载（2026-11-06 发布；更早那两个 `v0.1.0`、`v0.2.0` 已删除，对应的 tag 也删了）。
包版本串是**三个包同一个号** `0.1.0-r23`（`r` 号从 2026-11-06 起对齐，release 的 tag 就写这个完整串）；
包没签名，装的时候要 `apk add --allow-untrusted`，下完先 `sha256sum -c SHA256SUMS.txt`。
不想用发布出去的包也可以按 [DEVELOPMENT](docs/DEVELOPMENT.md) 第 8 节取最近一轮 CI 的 artifact（下载要认证，匿名 401），
或把本仓库登记成 feed、在 ImmortalWrt 树里自己编。

```sh
# 装：把三个 apk 放到机器上，一条命令装完 + 读回 + 冒烟
sh tools/dev/verify-install.sh /tmp/zte-apk        # 会真装包、真重启服务
sh tools/dev/verify-install.sh                     # 只读检查，不装不重启
```

LuCI → 网络 → **ZTE Mesh**。「常规」里选设备角色，「Wi-Fi 配置」填 2.4G 名称与密码（只有本机也发 Wi-Fi 时才用到）。
装完记得重启 `rpcd` 与 `zte-mesh`——上面那条命令已经做了。完整步骤、三种拓扑怎么组合、"设了没动静"的四种情况见
[docs/INSTALL.md](docs/INSTALL.md)。

## qemu-arm-static 安装指南

原厂控制器是 **32 位 ARM** 的程序。这台机器的内核不能直接执行它时，服务用它跑的是一份 `qemu-arm-static`
（用户态模拟器，不是虚拟机）。**包里不带它，也不会自动下载**——这是有意的取舍，理由见
[DEVELOPMENT](docs/DEVELOPMENT.md) 第 11.2 节。

**先判断这台到底要不要装**：

```sh
/etc/init.d/zte-mesh restart; sleep 5
logread | grep -e qemu -e execv -e launcher
ubus call luci.zte-mesh status
```

| 日志读到 | 含义 | 要做什么 |
|---|---|---|
| 什么都没有，`status` 里 `running=true` | 内核自己就能跑 ARM32 | 不用装 |
| `… no qemu-arm-static was found in /usr/lib/zte-mesh/root …` | 探测认定这台跑不了，且没找到模拟器 | 按下面装一份，或改走出路 1 |
| `execv errno=8` | chroot 之后 exec 那个 ARM32 文件失败（`ENOEXEC`） | 同上——**先按这一节处理，它不是代码 bug** |
| `zte-mesh has no launcher build for …` | 缺的是本机 CPU 那一份启动器，与模拟器无关 | 看上面「支持的架构」 |

**三条出路，按推荐顺序**：

1. **刷一台内核带 `CONFIG_COMPAT=y` 的机器**——真实路由器上唯一干净的解法，装了以后一个字都不用改。
   配置在 `.github/workflows/build-firmware.yml`（qualcommax/ipq807x，CI 自己编内核，因为 ImageBuilder 改不了内核选项）。
2. **手工放一份模拟器**（测试环境、临时验证用）。取**与这台设备同架构**的那一份——aarch64 设备要 arm64 版、
   x86_64 设备要 amd64 版，拿错了放上去也 exec 不起来：

   | 这台设备 | 取哪一份 deb（Ubuntu 24.04 / noble） | deb 里 `usr/bin/qemu-arm-static` |
   |---|---|---|
   | `x86_64`（amd64） | [qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb](http://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb)（deb 14 709 858 字节） | 4 250 456 |
   | `aarch64`（arm64） | [qemu-user-static_8.2.2+ds-0ubuntu1.18_arm64.deb](http://ports.ubuntu.com/ubuntu-ports/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_arm64.deb)（deb 16 942 584 字节） | 5 018 872 |

   上面两行是 2026-11-06 逐个下载解开来量过的读数（`http://ports.ubuntu.com/ubuntu-ports/` 那一份同目录
   [文件清单](http://ports.ubuntu.com/ubuntu-ports/pool/universe/q/qemu/) 里还有别的版本；想换发行版就从这个目录名往后接）。
   **只有 `8.2.2` 这种老一点的版本里才是真静态二进制**：`9.2.1+ds-1ubuntu5` 那一档（Ubuntu 25.10 及之后的更新）
   已经变成过渡包，整个 deb 只有 15 050 字节，`/usr/bin/qemu-arm-static` 只是指向 `qemu-arm`（动态链接、要靠
   一堆 `.so`）的符号链接——**放进私有根照样 exec 不起来**，因为沙箱里没有那些库。
   测试机上现在那两份是更早放的 11 MB 档（`owrt` 11 284 016、`ax3600` 11 244 456），比 noble 这份大一倍多，
   换不换由 flash 决定。

   ```sh
   # 在构建机（Ubuntu/Debian）上取一份：按设备的 CPU 二选一，别两行都跑
   curl -O http://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb   # 设备是 x86_64
   curl -O http://ports.ubuntu.com/ubuntu-ports/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_arm64.deb  # 设备是 aarch64
   dpkg-deb -x qemu-user-static_8.2.2+ds-0ubuntu1.18_*.deb /tmp/q
   ls -l /tmp/q/usr/bin/qemu-arm-static        # 要看到几 MB 的真文件；只有符号链接就是上面那档过渡包
   # 推到设备的私有根里（私有根默认 /usr/lib/zte-mesh/root，ZTE_MESH_ROOT 可改）
   scp /tmp/q/usr/bin/qemu-arm-static root@<这台路由器的IP>:/usr/lib/zte-mesh/root/qemu-arm-static
   ssh root@<这台路由器的IP> 'chmod 755 /usr/lib/zte-mesh/root/qemu-arm-static'
   ssh root@<这台路由器的IP> 'ls -l /usr/lib/zte-mesh/root/qemu-arm-static'   # 存在且可执行
   ```

   在 24.04 上 `apt-get download qemu-user-static` 取到的就是上面这一档；发行版更新之后它取到的是那个只剩符号链接的过渡包，
   所以要指定架构时照上面的链接拿文件。

   或者不动文件，指个宿主上的绝对路径过去——init 会先比 sha256，不同才拷进私有根的 `usr/bin/`
   （相同就不重复拷那 11 MB），重启服务时日志里留一句 `copied … into the private root as …`：

   ```sh
   uci set zte_mesh.main.emulator=/usr/bin/qemu-arm-static
   uci commit zte_mesh
   /etc/init.d/zte-mesh restart
   ```

   放好后确认这两条：`logread | grep -e qemu -e execv` 里不再出现 `no qemu-arm-static was found`，
   `ubus call luci.zte-mesh status` 回 `running=true`。
3. **`uci set zte_mesh.main.emulator=none`**——只用来确认"这块板子到底能不能原生跑"，它不会让服务起来。

**四个坑**：

- 模拟器是在 **chroot 之后**被 exec 的，所以沙箱看不见的文件等于没有：宿主上装了 `qemu-user-static` +
  `binfmt-support` **不解决服务的这个问题**（那两套只方便你在沙箱外面直接跑 ARM 程序）。
- 私有根里它依次找三个位置：`/usr/lib/zte-mesh/root/`、`…/usr/bin/`、`…/bin/`，文件名必须是 `qemu-arm-static`。
- 别放 `/tmp`——那是 tmpfs，重启就没了，而下一次启动又是一句 `errno=8`。
- `zte_mesh.main.emulator` 这个选项**不在页面上**，只能 `uci set`（或 `/etc/config/zte_mesh` 里手写）。

## 开发

```sh
sh build.sh                                          # 提取运行环境、审计、编译、主机测试、离线加载测试
sh tools/build/container-make.sh test-host           # 只跑主机测试
sh tools/build/runtime.sh install                    # 重新生成随仓库提交的运行包
python3 tests/test_i18n.py                           # 改了界面文案必须跑
```

分支、`prebuilt/`（`build-lab-tools.yml` 的 artifact，不入仓库）、CI（**手动触发**）与测试基线条数见
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | C 侧：沙箱启动器、服务、垫片与 preload 库、命令行工具，以及原厂表结构的 `.def`/`.inc`（职责见 [ARCHITECTURE](docs/ARCHITECTURE.md)） |
| `build.sh`、`Makefile`、`Dockerfile*` | 构建入口：提取并审计运行环境、交叉编译、跑主机测试 |
| `tests/` | 主机测试（C 编解码、Python、shell、LuCI 的 JS） |
| `openwrt/` | OpenWrt 包：`zte-mesh`（含按 CPU 各一份的启动器）、`luci-app-zte-mesh` |
| `prebuilt/` | 测试主机用的预编译件（`build-lab-tools.yml` 的 artifact），**不进仓库、不进任何 apk** |
| `tools/` | `tools/dev/` 是构建与测试用的 17 个 Python 脚本，另有构建容器与打包、抓包重放、子路由 telnet、测试环境里的脚本 |
| `docs/` | 文档（下） |
| `out/`、`evidence/` | 构建产物与验证证据，含原厂二进制，**不入库** |

C 侧在 2026-11-04 挪进了 `src/`：`Makefile` 逐文件列源、`tests/*.c` 的 `../` 包含前缀、`.def`/`.inc` 的同目录包含
都要跟着改，一共动了 74 个跟踪文件（51 个挪、23 个改）；随包分发的 `ns-run.c`/`controller-net.c` 用的是 tar 成员名，成员名一个没动。
Python 同日挪进 `tools/dev/`：`tests/`、`openwrt/make-runtime.py`、`tools/lab/`（当时还在）按模块名 import 它们，改的是同一批
`sys.path.insert` 加上 `build.sh`／`Makefile` 的调用路径；根目录只剩构建入口。

## 文档

**要跟着改动更新的四份**：

| 文档 | 内容 |
|---|---|
| [FEATURE_STATUS](docs/FEATURE_STATUS.md) | **功能总账**：逐项状态与判据 |
| [PLAN](docs/PLAN.md) | **唯一排期**：分批做什么、主机可测与需上机分开、上机清单 H-01…H-17 |
| [INSTALL](docs/INSTALL.md) | 用户手册：装包、页面、三种拓扑、验收、卸载 |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | 三台测试机清单、构建、打包、测试基线、上机守则、ARM32 模拟器判定与 `prebuilt/` 的取法 |

**原理与接口，改代码前读**：[ARCHITECTURE](docs/ARCHITECTURE.md)（结构与源文件职责、固件哈希）、
[SERVICE](docs/SERVICE.md)（UCI/LuCI/CM 映射与热更新）、
[REAL_CHILD](docs/REAL_CHILD.md)（中兴原厂路由器的配置语法、基线与红线）。

**只读资料，不再随结论更新**：[docs/research/](docs/research/)（原厂逆向与上机取证：功能映射与差距、重放计划与结果、
E2633 固件、agent 探测面、1905 冷加入时序），[docs/milestones/](docs/milestones/)
（早期里程碑的验收记录，字段级的 `SCHEMA` 与 `POLICIES` 在这里）、[FINDINGS](docs/FINDINGS.md) 与
[CONTROLLER_CAPABILITIES](docs/CONTROLLER_CAPABILITIES.md)（角色选举触发位、网页锁定公式、`sendcmd` 报文格式、
Z506 的能力矩阵）、[机型对照](<docs/ZTE router model.md>)。原厂固件镜像不在仓库里：`tools/dev/prepare.py` 默认读 `../zx279133/vendor-reference/2b5/rootfs.jffs2`，可用
`SR1010_VENDOR_ROOTFS` 指定。

## 约定

- 仓库里不放任何密码和密钥。子路由 telnet 口令只从环境变量 `ZTE_TN_PASS` 读；服务配置里的 Wi-Fi 密钥只写进
  0600 临时文件，用完删除。
- `evidence/` 和 `out/` 里有原厂二进制、WSC 材料和原厂整库，不入库，不外传。
- 原厂程序和库带旧 OpenSSL，产物只用于私有工程验证；对外分发前要另外处理安全维护、最小权限和二进制再分发许可。
