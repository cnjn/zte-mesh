# 装上并用起来

只回答一件事：**从一台刚刷好 OpenWrt / ImmortalWrt 的路由器开始，怎么把它和中兴路由器组起来**。原理见
[ARCHITECTURE](ARCHITECTURE.md)，每个选项的完整语义见 [SERVICE](SERVICE.md)，构建与测试命令和各套测试的条数基线见
[DEVELOPMENT](DEVELOPMENT.md)，测试机清单见 [LAB](LAB.md)——这里不重复它们，只给最短路径和判据。

目标是 [PLAN](PLAN.md) 第 1 节四个场景里的前两个：**S1 主控**（OpenWrt 路由器当主控，一台中兴原厂路由器不刷机、网线连过来）和
**S2 子节点（有线回程）**（OpenWrt 路由器挂在中兴原厂路由器下面当子节点，一根网线）。S3（无线回程）配置面已经就绪，**端到端还没在
真实路由器上验过**（[PLAN](PLAN.md) 的 H-01），第 6 节标了出来。

## 1. 动手前确认三件事

| 要确认 | 怎么看 | 不对会怎样 |
|---|---|---|
| **这台机器能不能直接跑 32 位 ARM 程序** | `uname -m` 是 `aarch64` 且固件带 `CONFIG_COMPAT=y` → 能。`x86_64` 或没有 compat 的 arm64 → 不能，要么自己把 `qemu-arm-static` 放进私有根 `/usr/lib/zte-mesh/root`，要么把 `zte_mesh.main.emulator` 设成**这台机器上的任意绝对路径**（比如 `/usr/bin/qemu-arm-static`）——服务会先比 sha256，不同才拷一份进私有根的 `usr/bin/`，拷不进去就不起并说清原因 | 控制器实例每 5 秒以 `execv errno=8` 退出，页面显示没跑起来。模拟器**不在任何包里**，判定表与三条出路见 [LAB](LAB.md) 第 4.2 节。多数固件不导出 `/proc/config.gz`（两台测试机都没有），所以这一条别看配置文件，看服务起来没起来、日志有没有 `errno=8` |
| **无线回程要完整 `wpad`** | `apk info wpad`（`wpad-openssl` 这类变体也算，它 provides `wpad`）。25.12 起的 ImmortalWrt 用 apk，**机器上没有 `opkg` 这个命令**，`opkg list-installed \| grep wpad` 只在老 release 上有意义；`wpad-basic` 不够 | 只有开 `backhaul`（第 5、6 节）才受影响；`wlan-sync` 会把原因写进日志与页面状态行，不会静默失败 |
| **flash 够不够** | `zte-mesh` 这个 apk 现在同时带运行环境和三个 CPU 的各一份启动器：合并后的 `zte-mesh-0.1.0-r22.apk` 是 **5 101 814 字节**（合并前那两份产物 3.9 MB + 1.2 MB，两份相加 5 104 550，所以合成一个还小了 2 736 字节）；LuCI 与中文包是 KB 量级。设备上以 `apk info -s zte-mesh` 为准——**装 r22 后两台测试机都读 10 MiB**（合并前 `zte-mesh` r21 单独是 8 034 KiB；更早文档里那个 8 007 KiB 只覆盖运行环境那部分，别拿它当合并后的数）。**要跑模拟器的那台机器上，占地方的是你自己放的那份 `qemu-arm-static`（10.7 MB），不是包** | 换 `wpad` 变体也可能挤爆小 flash |

## 2. 装包

三个包，名字与作用：

| 包 | 架构 | 内容 |
|---|---|---|
| `zte-mesh` | `all` | 服务本体：`/etc/init.d/zte-mesh`、`attach` / `wlan-sync` / `sta-collect`、rpcd 插件、默认 `/etc/config/zte_mesh`、带原厂 `multiapd` 的运行包（解到 `/usr/lib/zte-mesh/root`）。**启动器也在这个包里**：现编的 `ns-run` 与 `mesh-search`，**一个 apk 带三个 CPU 的各一份**——`/usr/libexec/zte-mesh/arch/{aarch64,armv7,x86_64}/{ns-run,mesh-search}`，而 `/usr/libexec/zte-mesh/{ns-run,mesh-search}` 这两个名字下面是同一份 shell 入口，按 `uname -m` 选目录、按 `$0` 选程序，选中就 `exec`（pid 不变）。依赖 `ip-full`、`kmod-veth`、`kmod-sched-core`。缺 veth 内核模块时启动器停在 `error=private-service-network errno=2`（建私有网线之前先 `stat /sys/module/veth`）。以前这两部分是两个包（`zte-mesh-launcher`），2026-11-05 合并——理由与代价见 [ARCHITECTURE](ARCHITECTURE.md) 第 5 节 |
| `luci-app-zte-mesh` | `all` | 配置页面（`网络 → ZTE Mesh`） |
| `luci-i18n-zte-mesh-zh-cn` | `all` | 中文文案。**装应用包不会自动带上它**，要中文就单独装 |

**手工装（最常用）**：这一批发在 **[GitHub Release `v0.1.0-r23`](https://github.com/Leenshady/luci-app-zte-mesh/releases/tag/v0.1.0-r23)**，
四个文件都能匿名下载（2026-11-06 发布；tag 指本次构建吃的树 `9696812`，不是它上面那份只改文档的提交）。
更早那两个 `v0.1.0`、`v0.2.0` 的 release 与 tag 已删除。另一条路是把本仓库登记成 feed、在 ImmortalWrt 树里编出来，
或者从 `build-apk.yml` 那次运行的 `zte-mesh-apk` artifact 取（下载 artifact 要认证，匿名 401；取法见 [DEVELOPMENT](DEVELOPMENT.md) 第 8 节）。
仓库当前构建的包版本串是三个包同一个号 `0.1.0-r23`（2026-11-06 起 `r` 号对齐，一批一个号，规矩见 [PLAN](PLAN.md) 第 6.3 节）；
`ci/batch24` 那一轮已经按这个号出过产物，三个文件名都带 `0.1.0-r23`，两台测试机装完读回三行同为 `0.1.0-r23`。
包没签名，所以要 `--allow-untrusted`：

```sh
# 1) 从 release 页面取这一批的四个文件（匿名可下），先在本机校验
mkdir -p /tmp/zte-apk && cd /tmp/zte-apk
BASE=https://github.com/Leenshady/luci-app-zte-mesh/releases/download/v0.1.0-r23
for f in SHA256SUMS.txt zte-mesh-0.1.0-r23.apk luci-app-zte-mesh-0.1.0-r23.apk luci-i18n-zte-mesh-zh-cn-0.1.0-r23.apk; do
  curl -sfLO "$BASE/$f"
done
sha256sum -c SHA256SUMS.txt                   # 三条 OK 才往下走

# 2) 推到路由器并装
ssh root@<这台路由器的IP> 'mkdir -p /tmp/zte-apk'
scp dist/*.apk root@<这台路由器的IP>:/tmp/zte-apk/     # 只放这一批；目标目录不存在时 scp 会把文件写成那个名字
ssh root@<这台路由器的IP> 'cd /tmp/zte-apk && sha256sum -c SHA256SUMS.txt'
ssh root@<这台路由器的IP> 'apk add --allow-untrusted /tmp/zte-apk/zte-mesh-[0-9]*.apk /tmp/zte-apk/luci-*.apk'
```

上面第 2 步的 `dist/*.apk` 是本机或 CI 构建出来的那份；从 release 下的四个文件在 `/tmp/zte-apk`，scp 那一行换成 `scp /tmp/zte-apk/*.apk` 就行。

- **`zte-mesh-[0-9]` 那个 `-` 加数字不是排版**：`zte-mesh*.apk` 会把这一台机器上**上一批留下的** `zte-mesh-launcher-*.apk`（合并前那个独立的启动器包）一并匹配进来，那条命令于是当场多装一个包——它装的是旧一半的启动器，而新的 `zte-mesh` 已经带着这些文件。装机读回 `verify-install.sh` 现在用同一条 glob，并且专门有一行会拒绝"这个目录里还有 launcher 包"。

- **一个目录只放一批**：`sha256sum -c` 只证明"文件没坏"，证明不了"这是这一批"（旧 apk 和它们的 `SHA256SUMS.txt` 一起留着时校验照样全 OK），而 glob 同时命中两批时 `apk` 解出来的目标版本连文件名都对不上（报 `Downgrading zte-mesh (0.1.0-r17 -> 0.1.0-r9)`，而目录里根本没有 r9 的文件）。换批前把旧 apk 挪走或改名 `*.apk.stale`，别指望校验和替你把关。
- 装与升级**不会覆盖**你改过的 `/etc/config/zte_mesh`（它是 conffile），但页面存过一次之后本地这份就和包里默认那份不同，于是**每次装都会在旁边留下一个 `/etc/config/zte_mesh.apk-new`**（8 KB 出厂默认配置）——它是 apk 的"这里有冲突"记号，不是备份也不是垃圾；想清掉就按第 8 节那条 `zte_mesh*` 的 glob 一起删。
- **架构不匹配导致整串 `apk add` 装不上，这一种失败从 2026-11-05 起没有了**：带启动器的那个包（现在就是 `zte-mesh`）是 `all`，什么机器都收，包里有 aarch64 / armv7 / x86_64 三份二进制。以前它是 `aarch64_cortex-a53`，在 x86_64 虚拟机上 `ERROR: unable to select packages: ... uninstallable` 会让另外三个 `all` 的包一起装不上（apk 求解是全有或全无），只能把启动器从命令里拿掉再装。**代价是那句拒绝搬到了运行时**：这台机器的 CPU 不在包里时，入口脚本会说出它站在哪台机器上和包里带的那几种架构——`zte-mesh ns-run: no build for this machine (uname -m says mips64). The launcher on this box carries: aarch64 armv7 x86_64`，`zte-mesh.init` 起实例前先问一次 `--launcher-check`，于是日志里是这一句，而不是 procd 每 5 秒重开一个 127。**这一趟也意味着手工编的 `ns-run` 会被覆盖**：`/usr/libexec/zte-mesh/ns-run` 现在是包里的入口脚本，自己编的那份要放进 `arch/<cpu>/`（见第 8 节）。只装 `zte-mesh` 以外的包 = 页面能打开、控制器起不来（`ns-run` 现在也在 `zte-mesh` 里，缺它等于没装这个包）：三个包是"一套"，不是可选菜单。
- **三个包放在同一条 `apk add` 里**（就是上面那行命令）。启动器和运行环境现在同一个 apk、同一次事务，`zte-mesh` 的 post-upgrade 重启服务时问到的入口脚本就是这一批的那份。以前分批装会留下一个空档：2026-11-05 在 `owrt` 上 `05:54:30` 先装 `zte-mesh` r21、`05:54:34` 才装 launcher r2，中间那次重启问到的是这台手工放着的旧 ELF，它不认识 `--launcher-check`、只打 usage 并回 64，日志于是记了一句假的 `zte-mesh-launcher has no build for x86_64: usage: ns-run …`（这台并不缺构建）。**架构不匹配只由 127 表达**，其余非零另有说明，所以那句假诊断的成因已经消掉两层：包合并了，init 也不再拿任何非零当架构拒绝。
- **这台机器上还装着合并前的 `zte-mesh-launcher`：先删它，再装合并后的 `zte-mesh`**。2026-11-05 在 `owrt` 上把这一条读了一遍，`/usr/libexec/zte-mesh/{ns-run,mesh-search,arch/**}` 那八个文件同时被两个包声称拥有，`apk add` 的答复是**逐条拒覆盖**——八行 `ERROR: zte-mesh-0.1.0-r22: trying to overwrite usr/libexec/zte-mesh/ns-run owned by zte-mesh-launcher-0.1.0-r2.`（两个名字 × 三种架构），**退出码 1**；可它同一趟把 `zte-mesh` 记成已安装、跑了 `post-upgrade`、也装上了新脚本。这台机器当场还能跑（旧那份入口脚本内容一样），但 `apk info -L zte-mesh` 只列 4 个 `libexec` 成员，入口和六份二进制仍归 launcher。**这种状态再去 `apk del zte-mesh-launcher`，入口就被它一并删掉了**：`ls /usr/libexec/zte-mesh/` 只剩 `arch`、`attach`、`sta-collect`、`wlan-sync` 四项，两个入口名字都不在，`--launcher-check` 回 **127**（ash 那句 `not found`），而 `apk add` 那个 apk 文件是**空操作**（包已经"装着"）、`--force-overwrite` 也一样。恢复只有把三个包一起 `apk del` 再装——单删 `zte-mesh` 会被拒（`not removed due to: zte-mesh: luci-app-zte-mesh / luci-i18n-zte-mesh-zh-cn`，那两个包依赖它）。**顺序反过来就全都干净**：`apk del zte-mesh-launcher`（rc=0，`OK: 41.8 MiB in 224 packages`）→ `apk add --allow-untrusted /tmp/zte-apk/zte-mesh-0.1.0-r22.apk`（`Upgrading zte-mesh (0.1.0-r21 -> 0.1.0-r22)`、rc=0）→ `apk info -L zte-mesh` 列 **12** 个 `libexec` 成员，两个入口名字和 `arch/{aarch64,armv7,x86_64}/` 六份二进制都归这个包了（`ax3600` 走的就是这条，`RESULT=PASS` 19 查 0 败）。

**编进固件**（长期用这台机器当控制器时更省事）：把本仓库当 feed 加进 ImmortalWrt 树，`./scripts/feeds update zte_mesh && ./scripts/feeds install -a -p zte_mesh`，`.config` 里勾上 `CONFIG_PACKAGE_{zte-mesh,luci-app-zte-mesh,luci-i18n-zte-mesh-zh-cn}=m`。feed 登记与编译命令见 [DEVELOPMENT](DEVELOPMENT.md) 第 8 节。这样出来的启动器只带**这棵树正在构建的那种架构 CPU**（构建树里只有目标机的工具链；另外两种架构要宿主的 `gcc-aarch64-linux-gnu` / `gcc-arm-linux-gnueabihf` 在场，CI 那次就是这么装的）。这不是问题——apk 装到哪台机器都收，包不带的架构由入口脚本在运行时说清楚，`ZTE_MESH_ARCHES` / `ZTE_MESH_REQUIRE_ARCHES` 可以分别限定"编哪几种架构"和"缺了哪种架构就报错"。

## 3. 打开页面并确认服务起来了

1. LuCI 登录 → **网络 → ZTE Mesh**（页面标题 `ZTE Mesh`，中文版是「中兴组网」）；「常规」tab 最上面是**启用**（默认开）与服务状态——状态框会写这台机器现在的角色和有没有在跑。
2. 命令行等价检查（页面打不开时先看这三条），它们和"重启两次"这条规矩各自答什么：

```sh
ubus call luci.zte-mesh status          # 返回 role / local / 实例是否在跑
logread | grep zte-mesh | tail -30      # 起不来的原因都写在这里
cat /run/zte-mesh/wlan-sync.status      # 配置写进本机无线配置的结果（只有会把配置应用到本机的角色才有）
```

| 看什么 | 它答什么 | 什么情况不算故障 / 不作判定 |
|---|---|---|
| 配置被拒绝：`logread` 一行 `daemon.warn zte-mesh: <哪个选项> ...; not starting`，`status` 变成 `running=false` / `attached=false` / `age=-1` / `state=null`，`wlan-sync.status` 干脆不存在 | 服务根本没起来——看到 `state=null` 就别再读后面那些键 | `ns-run` 打的 `ZTE_CONTROLLER_TRACE unix_bind ... errno=98/111` 是套接字重绑的过程记录 |
| `wlan-sync.status` 的 `bands=` | **这一次把配置写进本机时改动了哪些频段**，不是"这台机器只管这些频段"：可能只写 `bands=5g` 而两个频段的名字都是这次配置里带的（日志一行一个频段说清楚） | — |
| **装完重启两次**：`/etc/init.d/rpcd restart` 和 `/etc/init.d/zte-mesh restart` | `apk add` 只换文件，rpcd 还拿着装之前加载的那份插件（状态里连 `local` 这个键都没有）、`attach` 报着 running 而 `zmesh0` 早就不存在；重启之后 `attached=true`、`zmesh0` 重新挂回 `br-lan` | — |

这三条加"实例集合对不对、主控给的 SSID 有没有写到本机"就是装机验收的一条命令：把 apk 放机器上 `/tmp/zte-apk`，跑
`sh tools/lab/verify-install.sh /tmp/zte-apk`（不带目录只读不装）。它输出 `KEY=VALUE` 证据行和末尾 `RESULT=PASS|FAIL`，
判定逻辑由 `sh tests/test_verify_install.sh` 在主机上守着；**带目录那种架构会装、会重启服务**，读到的是这一批跑起来的样子。
每个键的意思只在这里写一次：

| 键 | 它答什么 | 不作判定的情况 |
|---|---|---|
| `ssid_wait=<秒>` / `ssid_pending=0 1` | 主控的 SSID 有没有写到本机无线：该项会**等配置写进本机并生效**再判（上限 `SSID_WAIT`，默认 60 秒），等不到才 FAIL 并列出没生效的行号 | 刚重启的那十几秒里无线配置可能还是上一个名字——问得太早会把一台健康的子节点判成 FAIL |
| `trace_per_bss_name`、`probe_bss_detail`、`init_seed_from_air`、`sync_flag_by_meaning`、`collect_pause_in_collector` | **这一批有没有真装进机器**。apk 打包会剥掉 ELF 节，设备上的 `.so` 和仓库成员不是同一串字节，只能按字符串认：前两个 `yes` 说明**写入方**在机器上；后三个认的是包里三个 shell 文件（`/etc/init.d/zte-mesh` 的 `live_ap_iface`、`/usr/libexec/zte-mesh/wlan-sync` 的 `set_flag`、`/usr/libexec/zte-mesh/sta-collect` 的 `collect-pause`）——**一批只改脚本时 rootfs 里一个字都没变，只读那两个库就会把"没装上"读成"装上了"** | 机器上没有那份文件时打 `unreadable`、不作判定（环境事实不是批次判定）。`peers_sta` / `peers_software` / `peer_reports_stations` / `ap_sta_rows` / `ap_sta_age` / `state_bss_detail` 是读到的数据，只当证据不当判据 |
| `ap_sta_rows=<个数>` / `ap_sta_age=<秒>` | **这台子节点的采集循环有没有在跑**。表是 `/run/zte-mesh/ap-sta`（`epoch=<秒>` 一行，再一路 AP 一行 `BSSID 数`）：`ap_sta_rows` 数里面的行，`ap_sta_age` 是现在减那个 `epoch`。年龄一小就说明还在更新时间戳，年龄大到越过应答那 30 秒的窗口就说明循环停了或工具没了——**沉默是设计**，跟原厂 agent 收到查询不回一样，报一个陈旧的数才是编造 | 表读不到 → 两行都 `absent`（主控本来不写这张表，不是故障）；`epoch=` 那行没了 → `ap_sta_rows` 照数、`ap_sta_age=unreadable`。暂停开关 `touch /run/zte-mesh/collect-pause` 之后年龄会一直涨、行数不动，这就是"停了但表还在"的样子（[SERVICE](SERVICE.md) 的 `sta-collect`） |
| `peer_reads=<次数>` / `peer_input=yes\|no` | **主控这一趟到底有没有收到过终端数**（[PLAN](PLAN.md) H-13、H-15）。脚本连读快照直到有一处读到数为止，最多 `METRICS_READS` 次（默认 3，每次隔 5 秒）：`peer_reads` 是读了几次，`peer_input` 是 `.sta` 文件存在**或**摘要里有 `stations=`——两条来源任一成立都算有输入 | 只当证据。`peer_input=no` 分不出"这台没人上报"和"这台不在了"，那要看节点还在不在 `nodes_rows=`；替身按 BSSID 应答、不发摘要那一段，所以 `peers_sta=2` 旁边写 `peer_reports_stations=no` 是正常形态 |
| `launcher_dispatch` / `launcher_second_name` / `launcher_arch_present` + `launcher_arch=<入口那一行>`（另有只报不判的 `legacy_launcher_package=`） | **这台机器有没有一份跑得动的启动器**（[FEATURE_STATUS](FEATURE_STATUS.md) C12）。启动器现在在 `zte-mesh` 这个 `all` 的包里，装机时不再挑架构，那句答案就搬到这里来了：`launcher_dispatch` 认 `/usr/libexec/zte-mesh/ns-run` 是不是那份入口脚本（按字符串 `--launcher-check` 认——apk 打包会剥掉 ELF 节，比字节没用），`launcher_second_name` 认同一个脚本的第二个名字 `mesh-search`（服务真会跑它，少了就是 search 实例起不来），`launcher_arch_present` 是问出来的那一行**退出码是不是 0**，也就是本机 `uname -m` 那种架构在不在包里。`launcher_arch=` 打印入口自己回答的第一行：有构建时是它选中的那个文件（`/usr/libexec/zte-mesh/arch/x86_64/ns-run`），没有时是那句"这台机器不在包里，包里带的是 `aarch64 armv7 x86_64`"——**问的就是装机要跑的同一份代码**，人手工跑 `ns-run --launcher-check`、init 记进日志的那行、这里读回来的是同一句话。`legacy_launcher_package=` 报这台机器上**还留着合并前那个独立 launcher 包**的版本号（没有则 `absent`）——两个包同时声称拥有 `/usr/libexec/zte-mesh/` 下那批文件时会发生什么是 apk 的判定，只报不判（[PLAN](PLAN.md) 第 5 节）。入口不在、读不到、或不是一份 dispatcher（比如手工放的旧二进制）→ 两行只报 `launcher_dispatch=<unreadable\|no>` 与 `launcher_arch=not-installed`，**不作判定**：环境事实不是批次判定。**"这台机器的 CPU 不在包里"是要判 FAIL 的**（`launcher_arch_present=no`），这是拿掉装机时那道架构门之后唯一的守门人 |
| `applier_writes=<次数>` | 这一次重启里 `applier`（把配置写进 `/etc/config/wireless` 的那段）**真的写过几次配置**，即日志 `applied; wireless committed` 的行数增量。**比文件不行**：旧的初始配置先把本机 SSID 改掉、主控再改过来，`/etc/config/wireless` 前后一模一样而家里已经掉了两次线——只有 applier 自己那行留了痕 | 判 `0` 的前提是重启前本机广播已是配置里每个启用行的 SSID（`air_matched_plan=yes`），不是 `yes` 就只报数不作判定；这一趟没重启 `no-restart`、这个角色本来就没有 applier `no-applier`（纯主控）、`logread` 读不出来 `no-logread`（当成 0 行，而 `0 - 0` 正长得像一次安静重启）、日志环在窗口里绕了一圈 `wrapped`（窗口被截断，增量不是个数） |
| `cur_hop=<数字>` / `cur_hop_age=<秒>` / `max_hop=<数字>` | 守护进程自己那行 `ZTE_MESH_SERVICE cfg` 里的跳数。子角色的启动路径只写 `-2`（"还没接入"），往子节点方向再没有第二处写它——**读到别的数就说明这个原厂守护进程真的更新了它**。`max_hop` 是同一行的另一列，摆在一起才看得出"子节点跟随主控给的数"还是"设成本机 `MaxHop`" | 当主控那一列是我们自己写的，只当证据；那一行已被日志环挤出去（`no-cfg-line`，测试机跑久了就是这样）、守护进程读不到那一列（`-98`）、这一趟刚重启过服务（新的那行可能落在我们读之后）、行末 `epoch=<秒>` 算出的年龄小于 `CUR_HOP_GRACE`（默认 120 秒——子节点重启后要在"还没接入"上待约半分钟）。**没有 `epoch=` 的行算不出年龄，照判**——不能因为读不到时刻就悄悄放过这一项 |
| `db_profile_cur_hop=<数字>` / `absent` + `master_cur_hop=<数字>` | **测试环境里那个开关有没有真进到主控自己的那一行**（[PLAN](PLAN.md) H-14 第 ⑦ 步）。开关不是页面上的选项：往 `/run/zte-mesh/db-profile` 写一行 `MultiAPCfg 0 CurHop 5` 再重启服务，启动视图会把它读进数据库（`mesh-policies.c` 的 `profile_apply`），而主控启动路径本来不写 `CurHop` 这一列，所以它能活过一次重启。`db_profile_cur_hop` 报文件里那一行（没文件、没这一行都算 `absent`），`master_cur_hop` 拿 cfg 那行的 `cur_hop` 跟它比，**相等才算开关生效**——不然子节点读到 2 会看起来像一个结论，实际上主控根本没把 5 放进数据库 | 文件在但角色是子节点 → `not-master`（子节点启动路径会把 `-2` 写回来，它后来那个数是守护进程对本机广播状态的确认，归 `cur_hop` 那条判定）；环里没有那一行 → `no-cfg-line`；那一列读不到 → `not-read`；那一行太年轻（同一道 `CUR_HOP_GRACE`）只报告数值、不作判定。**`absent` 时整条不作判定**，正常机器就是这一种 |
| `mesh_compat_uci` + `mesh_v2_line` + `mesh_compat_inverted` | **页面上那一项与守护进程读的那一行是不是反的**（[FEATURE_STATUS](FEATURE_STATUS.md) A12b）。`mesh_compat_uci` 是 UCI 里的 `mesh_compatible`（页面值），`mesh_v2_line` 是 `/run/zte-mesh/config` 里 `mesh.v2=` 那一行，判据是 `0→1`、`1→0`——反相只发生在 init 写那一行时，这里是唯一能在现场核对它的地方。`mesh_v2_legacy_uci` 报旧键 `mesh_v2` 还在不在（升级留下的配置常见 `0`），只当线索；`init_reads_mesh_compatible` 是包内脚本标记，认的是 `/etc/init.d/zte-mesh` 里那个新键名 | 页面没值（`unset`）而服务也没那一行 → **整条不下结论**：init 对没设过的选项不发 `mesh.v2`，守护进程用自己的默认（关＝接受任何子路由），这与旧键的默认是同一个行为，不是判定。`/run/zte-mesh/config` 读不到 → `mesh_v2_line=no-config`（控制器没跑起来，没东西可比）。**2026-11-04 两台测试环境机器装 r19/r11 就是这种“不下结论”的形态**：`/etc/config/zte_mesh` 被页面存过 → apk 留 `zte_mesh.apk-new` 而保留旧配置 → 新键 `unset`、旧键 `'0'`、服务无行 |
| `backhaul_asked` + `backhaul_{mode,multi_ap,wds}_5g` + 六行 `backhaul_{iface,assoc,4addr_iw,4addr_ip,brport,station}_5g` | 无线回程（H-01）那六步里的**前四步**，一次连读。判的只有我们写的那三行 uci：`mode=sta`、`multi_ap=1`、`wds=1`（缺 `wds` 直接 FAIL，因为 netifd 会 `BRIDGE_NOT_ALLOWED`）。射频答的六行只报不判：接口在不在、`iw dev info` 有没有 `ssid` 行（关联上了才有）、`iw`/`ip` 两条 4 地址 grep、挂在哪个桥、`station dump` 的 `Authorized` | 只在本机是 `slave` **且** `backhaul=multi_ap` 时判那三行；`backhaul_asked=off`（默认）整段不触发。**那两条 4 地址 grep 顺带也是一次实测**：仓库一直写"`iw dev info` 打 `4addr on`"却没有实测出处，哪个构建会这么说，读一次就知道——r19 那次两台都是 `off`，这次实测还没有读数 |
| `peers_seen` / `sta_file_sum` / `peer_software_bytes` / `bss_detail_rows` / `nodes_rows` / `nodes_online` | H-12（子路由发不发 `0xd4`）与 H-13（`0x800c` 一帧列全还是一射频一帧）的读数，**每个计数都配一个成立条件**：`peers_seen` 是 `peers/` 里出现过几台 MAC，`peer_software_bytes` 是 `.software` 一共几个字节，`bss_detail_rows` 是 `state.json` 里 `"bssid":"` 出现几次，`nodes_rows`/`nodes_online` 是节点行数与在线数 | 全部只报不判。`peer_software_bytes=0` 在 `peers_seen=0` 的机器上不说明 Z506 不发；"这一趟有没有输入"看上一行的 `peer_input`（它把 `.sta` 文件的个数与摘要里那行合起来判），**别看单次采样的 `peer_reports_stations`**——它读的是 `.metrics` 里那行 `stations=`，而 `.metrics` 只存最后到的那一帧，一帧不带终端数的状态帧会把这行抹掉只剩 `hops=`，下一次上报又写回来（2026-11-05 两台装 r20/r11 后八次连读，读到一次 `no`，同期 `peers_sta=2`、`bss_detail_rows=2`）。子角色本来没有自己的 agent，`state.json` 里没有 `bss_detail` 那把键（`state_bss_detail=absent`）、`nodes_rows=0`——这是结构，不是故障 |

开关只在 `/run` 下的一个文件里：**没有任何页面、UCI 选项或 apk 能设它**，所以到不了用户的机器。
测完删掉那一行（或整个文件）再重启服务即可。**一个坑**：在页面上把角色来回切会经过 `controller-service.c`
那处换角色的写列，往主控方向把 `CurHop` 写回 2——覆盖掉开关，直到下一次重启才重新生效；所以测这一条期间别动角色。

只想驱动这一次重启来问 `applier_writes`：`RESTART_NOW=1 sh tools/lab/verify-install.sh`——**它会真掉线**，默认关着
（判据见第 6 节和 [PLAN](PLAN.md) H-14）。

## 4. 场景 S1：OpenWrt 路由器当主控，接一台中兴原厂路由器

**这是出厂默认**，装完不用改角色：`zte_mesh.main.mesh_role=master`、`apply_local=0`（两个开关各自的完整语义见 [SERVICE](SERVICE.md) 的「设备角色（两个开关）」）。`uci show zte_mesh | grep apply_local` 很可能**一行都不出**——这一项没写就是 0，页面只在把它打开时才写进去；别把"读不到"当成"文档写的默认不存在"。

1. 页面决定**要不要自己也发 Wi-Fi**（这就是第二个开关）：**关**（默认）= 只当主控、把配置下发给子路由，**自己的无线一个字节都不动**（ZXSLC SR1010 这类刷 OpenWrt/ImmortalWrt 的有线网关选这个）；**开** = 同一份配置既下发给子路由、也写进本机 `/etc/config/wireless`，即原厂页面里的"主路由"——这时必须去「Wi-Fi 配置」tab 填 2.4G 名称与密码（名称必填、密码 8–63 字符），默认值 `ImmortalWrt` / `Ztemesh2631!`，**上线前改成自己的**。
2. 保存并应用。改这两个开关会让服务重启一次（实例集合变了），子路由会短暂掉出拓扑。
3. 子路由**保留原厂固件、不刷机**；它自己界面上的 MESH 开关与「MESH 兼容」**由用户自己设，本插件从不改动**（红线，见 [REAL_CHILD](REAL_CHILD.md)）。要它入网只需要：和这台 OpenWrt 在同一个二层（默认挂 `br-lan`，不是就改页面上的「LAN 桥」），然后**重启一次**子路由或等控制器下一次广播搜索（每 300 秒一次，最坏等 5 分钟）。
4. 「MESH 兼容」（`mesh_compatible`，主控侧的准入开关，名字与极性都跟子路由原厂页面一致）默认**开** = 接受任何子路由；**关** = 只接受市场码相同的中兴原厂路由器。接不上时先确认这一项。这个开关以前叫「自组网」（`mesh_v2`），**意思正好相反**：旧选项升级后被忽略，所以 r18 上把「自组网」设成开（只收中兴）的机器，升到 r19 会变成"接受任何子路由"——要恢复原来的严格准入，把这个开关**关掉**（服务键 `mesh.v2` 不变，仍由 init 反相写入）。

**判据**（S1 怎么算过）：页面「Wi-Fi 配置」改 SSID / 密码 / 信道 / ACL，子路由广播的 SSID 跟着变；节点表里这台子路由显示 `ZXHN Z506` 这类型号、状态在线，「重启」与「开灯 / 关灯」两个按钮点得动（只中兴原厂路由器有）。这条**要有中兴原厂路由器才能判**：测试环境里主控的对端是另一台装了本插件的 ImmortalWrt，报的是插件自己的型号字符串，按不了"显示 `ZXHN Z506`"这句话，改 SSID 那部分又会把家里正在用的无线配置改掉——两边都记在 [PLAN](PLAN.md) 第 5 节。

## 5. 想让它连访客网络、额外 SSID

「本机也启用额外 SSID」（`extra_slots`，默认关）+「额外的 Wi-Fi 网络」列表（每条选频段、网络序号、开关、名称、密码；序号 1=访客、2=备用、3=隐藏回程，同一频段必须按 1、2、3 连续，去掉 1 而留着 2 会被拒绝）。开与关的区别只在**会不会把这些网络写到本机射频上**，下发本身不受它影响。注意：额外网络和该频段的主网络**共用同一个网桥**，这个镜像还没做 VLAN 隔离，所以访客网络并没有和主网络隔开。

"被拒绝"是哪种拒绝（只留序号 2、不给序号 1）：服务**整个不起来**，`logread` 一句 `zte_mesh_bss '<段名>': ...; not starting`（序号这条是 `24g slot 2 needs slot 1 (slots must be 1..k); not starting`），`status` 是第 3 节那个 `state=null` 情况——页面存得下去，但服务不认，改错了先看日志那一行别猜。手敲 UCI 时 `uci add zte_mesh zte_mesh_bss` 只回一个段名（`cfg02306b`），要写成 `uci set zte_mesh.cfg02306b.band=24g`，漏掉 `zte_mesh.` 前缀就是五个 `uci: Entry not found` 加一个空段。

## 6. 场景 S2：OpenWrt 路由器当子节点，主控是中兴原厂路由器

1. 「常规」→ **设备角色** 选「子路由」。页面随即只留下这台机器用得上的几项（Wi-Fi / 漫游 / 额外网络的配置消失，因为配置由主控下发）。
2. 网线插到主控的 LAN 口。入网时**先沿用本机正在广播的无线**，等主控下发自己的名称/密码/加密——所以不需要在 OpenWrt 上填任何 Wi-Fi 参数。"沿用"是**逐频段从本机当前广播的配置读回来**的：服务起来前 `zte-mesh.init` 看这块板子每个频段正在广播的那一段，把名称、密码、加密、隐藏、开关原样当初始配置写进服务配置，于是主控的配置写进来时 `wlan-sync` 逐字段一比**没有要改的**——不 `commit`、不 `wifi reload`（`wifi reload` 是全局的：任何一个频段上的一次空写就把所有频段的终端一起踢掉）。两个频段广播的不是同一对时，各频段按各自的样子写，并把 `wifi_dual_band_sync` 写成 0，免得守护进程把 2.4G 的名称与安全派到 5G 上；页面上那对 `wifi_24g_ssid` 若与本机广播的 SSID 不同，日志会明说以本机广播为准（`wifi_24g_ssid is not the seed on this router`）。
   **拿页面那一对当初始配置的旧包（r17 及以前）每次重启会把本机广播的 SSID 改两次**（头几秒广播本地那份，约 21 秒后 M2 到了才翻成主控的名字），家里有人连着时就是两次掉线；仓库版本要的是一次都不翻，判据就是第 3 节那两个读数：`air_matched_plan=yes` + `applier_writes=0`（同一趟还报 `sync_changed=0`、`/etc/config/wireless` 的 md5 前后不变）。**要主动驱动这一次重启**：把脚本传上机器后跑 `RESTART_NOW=1 sh /tmp/verify-install.sh`（只重启、不装包），它属于第 9 节说的"先确认你能接受设备掉线"。
3. 判据（S2 怎么算过）：**中兴原厂路由器当主控**时改 SSID，20 秒内在这台机器上 `iw dev` 看到的名称跟着变，改回来也能回退。OpenWrt 路由器这边能测的是"本插件当主控"这部分（从子节点服务起到主控的名字改过来实测 21 秒，与"20 秒内"同量级）；中兴原厂路由器当主控的那部分还在等上机窗口。

**无线回程（S3，未端到端验证）**：同一页会出现「回控制器的方式」，选「Multi-AP（4 地址）」后，主控下发的回程网络会写成一个客户端接口（`zte_backhaul_sta_<band>`，`mode sta` + `multi_ap 1` + `wds 1`）。它要求完整 `wpad`，且**拔掉网线还能不能连上、1905 帧能不能过无线，目前只有配置面的证据**（[PLAN](PLAN.md) H-01）。当主控的那一端在同一页上叫「给子路由的回程网络」，把回程做成独立 BSS（`multi_ap=2`）、主 SSID 标成前传。

## 7. "我设了却没动静"的几种情况

共同点：**选项写了，不代表这个角色的服务会用它**。角色决定两件事——页面上出现哪些字段，以及这台机器上有没有一个
applier 会把配置写进本机射频；换过角色或手改过 `/etc/config/zte_mesh` 的机器，最容易出现"配了没动静"。逐条对照：

| 现象 | 为什么 | 怎么办 |
|---|---|---|
| 服务起不来，日志 `execv errno=8` | 内核跑不了 32 位 ARM 程序，且没有模拟器 | 第 1 节第一条（owrt 上实测：`ZTE_DB_SANDBOX error=execv errno=8` 每 5 秒一条，页面状态这时还是 `running=true`，只有 `attached=false` 这一项能看出异常） |
| 服务起不来，日志 `zte-mesh has no launcher build for <本机 uname -m>: zte-mesh ns-run: no build for this machine (… ). The launcher on this box carries: aarch64 armv7 x86_64` | 这台机器的 CPU 不在这个包里。**包是 `all` 的，`apk add` 什么机器都收**（2026-11-05 之前它会在这里直接失败、四个包一个不装），所以那句拒绝改由启动器在运行时说；`zte-mesh.init` 在把实例交给 procd 之前先问一次 `--launcher-check`，**只有回 127 才写成这一句**——其余非零是另一行 `--launcher-check exited <码>`，不会冒充"这台不缺构建"。问不到就不起，否则 `respawn` 会每 5 秒重开一个 127，而日志里从头到尾不出现"架构"两个字**这一句在 r23 之后才干净**：`zte-mesh.init` 先把入口文件在不在单独问一次，缺文件的那种不再冒充这一句（见下一条）| 换一台 aarch64 / armv7 / x86_64 的机器，或给这种架构 CPU 现编一份放进 `/usr/libexec/zte-mesh/arch/<cpu>/`（`tools/build/launcher-arches.sh`，命令见 [LAB](LAB.md)）。装机读回把这一条判成 FAIL（`launcher_arch_present=no`），`launcher_arch=` 那行就是它给的原文 |
| 服务起不来，日志 `… --launcher-check exited <非 127 的码>: …` | 入口脚本自己没答上这个问题。最常见是 `/usr/libexec/zte-mesh/ns-run` 站着的不是包里那份：批 20 之前手工放的旧 ELF 不认识 `--launcher-check`，只打 usage 并回 64（`owrt` 在 2026-11-05 分批装的那 4 秒里读到过一句假的架构拒绝，就是这么来的） | 重新 `apk add` 这个包（安装规则会覆盖这两个名字），或直接 `sh /usr/libexec/zte-mesh/ns-run --launcher-check` 看它说什么。**这一行不是"这台跑不了"**，别按上一条换机器 |
| 服务起不来，日志 `<入口路径> is missing from package zte-mesh` | 包少了一个文件——`/usr/libexec/zte-mesh/ns-run` 这个（和它的第二个名字）根本不在这台机器上。`apk` 的卸载会把两个名字一并删掉：`owrt` 在 2026-11-05 那次叠加态里就是这么丢的，当时它答的是上面那句假的"这台不缺构建"（ash 对不存在的文件回 127，和架构那个码一模一样），r23 起改成先问文件在不在、分两句说。**这一句在 2026-11-06 上机读过一次**（`owrt`，把 `ns-run` 暂时挪走再 `start`）：`daemon.warn zte-mesh: /usr/libexec/zte-mesh/ns-run is missing from package zte-mesh`，同时 `/etc/init.d/zte-mesh status` 是 `active with no instances`；同一台把 `arch/x86_64` 挪走读到的是上一条那一句，两句不串台 | `apk add --allow-untrusted` 重装这个包（安装规则会同时装回两个名字）；**先确认这是"包缺一半"而不是"这台不行"**，别按上一条换机器 |
| 回程开关设了，射频上什么都没有 | ①**手改 UCI 写进 `multi-ap` 这类第三种写法，不是"读成 off"，是整个配置被拒绝、服务不起**（`zte-mesh.init` 的取值表只认 `off` / `multi_ap`；owrt 上实测日志 `backhaul must be off or multi_ap; not starting`，状态变第 3 节那个 `state=null` 情况）——页面保存同样的写法是另一回事，它会当场拒绝并说明原因；②值本身合法但这台机器是不把配置应用到本机的主控，本来就不动射频：日志会明说 `zte_mesh.main.backhaul=multi_ap has nothing to do while apply_local is off: ...`（owrt 实测），并把两条出路写在那一句里 | 用页面而不是编辑器改它；确认这台会把配置应用到本机（角色是子路由，或者（「本机也广播」开着，或角色是子路由）。看页面「状态」与 `logread` |
| 一个频段有两块射频，改了却没生效 | 控制器配置不知道该写到哪块射频，`wlan-sync` 会**放弃这个频段**，日志原文是 `several 5g radios (radio0 radio1) and no way to tell which one carries the mesh; set zte_mesh.main.wifi_5g to one of them, this band is left alone until then`——它指定的是 UCI 项，页面上对应「2.4 GHz 射频设备」/「5 GHz 射频设备」两个字段 | 按日志列出的候选名填进那两个字段（例如 `radio1`）。保存这两项会重启服务，因为不重启就等于没选。靠 `radio0.disabled=1` 把候选缩成一块的板子（测试环境的小米 AX3600 就是这样，见 [LAB](LAB.md) 第 2.1 节）测不出这一条，别把"没报错"当成"已验证" |
| 有线口显示"未接入" | veth 两端一起没了，只有重启实例才能重建 | `attach` 兼任看门狗：连续 20 秒看不到这个口就重启沙箱实例，owrt 实测删掉 `zmesh0` 后 25 秒自己长回来（[PLAN](PLAN.md) H-10，已验）；想立刻恢复就重启服务 |
| 角色是子路由，手改的 `wifi_24g_ssid` 没动静 | 子路由的配置由主控下发，而服务起来时的**初始配置**是从本机当前广播的配置读回的（第 6 节第 2 条）。这几项在子角色上顶不上本机正在广播的那一段，页面上也根本不显示它们（[FEATURE_STATUS](FEATURE_STATUS.md) B9）；日志会明说 `child role: wifi_24g_ssid is not the seed on this router: ...` | 想改 SSID 就改**主控**的配置；想把这台变成能自己定的，就改成主控并开「本机也广播」。手工改 `/etc/config/wireless` 也能生效，但下一次 M2 到达时会被主控的配置盖掉 |

## 8. 卸载与版本

```sh
/etc/init.d/zte-mesh stop && /etc/init.d/zte-mesh disable
apk info -e | grep zte                 # 先看清这台装过哪几个
apk del luci-app-zte-mesh zte-mesh     # 再把装过的包名补进这条：luci-i18n-zte-mesh-zh-cn；这台若还留着合并前的 zte-mesh-launcher，也在这条里
```

- **一条 `apk del` 里只要有一个包名不在这台机器上，整条就失败**（`ERROR: No such package: luci-i18n-zte-mesh-zh-cn`、`rc=1`，
  一个包都没被删——没装中文包的机器上就是这样）：按实际装过的分开删，或先 `apk list --installed | grep zte` 看清楚（**`apk info -e` 在 `apk-tools 3.0.5` 上回 0 行**，两台测试机 2026-11-05 都读了，别拿它当"这台没装东西"）。
- **升级前就装着 `zte-mesh-launcher` 的那台机器：先 `apk del zte-mesh-launcher`，再装合并后的 `zte-mesh`**。
  这一条 2026-11-05 在两台测试机上各读了一次，两种顺序的结果都写在第 2 节那一条里。反过来做（先装合并包、再删 launcher）会把两部分留在不干净的状态：
  合并包记成已安装却只拥有 4 个 `libexec` 成员，删 launcher 时把入口脚本一并删掉，而再 `apk add` 一次那个文件是**空操作**（包已经"装着"）、`--force-overwrite` 也一样——
  恢复要连 `luci-app-zte-mesh` 与中文包一起删（`apk del zte-mesh` 单发会被拒：那两个包依赖它）。
  `legacy_launcher_package=` 那一行就是装机之前先看这一眼用的（这台还留着 launcher 包就报它的版本号，没有就报 `absent`）。
- apk 清不掉的要手工删：`/etc/config/zte_mesh*`（受保护的配置，apk 故意留着，第 2 节说的 `.apk-new` 也在这一条里）、
  `/run/zte-mesh`（tmpfs，重启就没了）。**`/usr/lib/zte-mesh` 那棵运行环境树是包自己装进去的，`apk del` 会连它一起删**——
  删完只剩你手工放的 `root/qemu-arm-static`（10.7 MB，占地方的一直是它，不是包）和那个空目录。
  **`/usr/libexec/zte-mesh/` 这两个名字现在归包**：`ns-run` 与 `mesh-search` 是同一个包里装的同一份入口脚本，
  它按 `uname -m` 去找 `arch/<cpu>/` 里的二进制，`apk add` 会覆盖你手工放在这两个位置的任何东西，`apk del` 把入口和
  `arch/` 一起删掉。自己编的那份要放进 `/usr/libexec/zte-mesh/arch/<cpu>/ns-run`（同名两个文件都要，见
  [LAB](LAB.md) 第 4.3 节），放在外面这一层的那份不会被入口脚本找到；`attach` / `wlan-sync` / `sta-collect` 和启动器在同一层目录里，
  但在同一个包里由同一条安装规则放下。
- 装的是哪一批从包版本读：`apk list --installed | grep zte` 报的就是 `pkgver`（与文件名里那一位一致）。三个包的
  `PKG_RELEASE` 各自计数、批末对改过的包加一（[PLAN](PLAN.md) 第 6.3 节；当前各包到了哪一位记在
  [FEATURE_STATUS](FEATURE_STATUS.md)）。不加这一位，装机本身不会失败——`apk add` 给同名同版本的 apk 也会
  `Replacing (0.1.0-r7 -> 0.1.0-r7)`——不动的是**记录**：`apk list` 报不出这台装的是哪一批，走 feed 的 `apk upgrade`
  更不会把它当升级推给你。批末不加号，等于这批只在构建机上存在过。
- 产物是 `build-apk.yml` **手动触发**跑出来的，push 不会自动构建；发布见 [DEVELOPMENT](DEVELOPMENT.md) 第 8 节。

## 9. 别做这些事

- **不改子路由的模式**（`MultiAPCfg` 的 `Mode`/`VersionMode`/`Enable`（MESH 开关）与 `P1905ComCfg.Enable`（MESH 兼容））——那是用户在子路由上自己设的。
- 向中兴原厂路由器推配置会**真实且持久**地改变设备设置；改 SSID、关主 SSID、ACL 封禁真实终端、信道/功率之前先确认
  你能接受设备掉线，并留着还原的基线（[REAL_CHILD](REAL_CHILD.md) 末尾有读回命令与基线）。
- **重启这个服务也算在上面那一类**：拿页面值当初始配置的旧包（r17 及以前），子节点每重启一次本机广播的 SSID 改两次、家里连着的人
  掉线两次（第 6 节）；初始配置改成从本机当前广播的配置读回之后，读到那一次的那台机器上重启一次都不写无线。**这句话只担保它自己那台
  机器那一批**：换机型、换批次，先跑一次带重启的验收（`RESTART_NOW=1`，[PLAN](PLAN.md) H-14）看到
  `air_matched_plan=yes` + `applier_writes=0`，再决定要不要在有人连着的时候动。
- 仓库里不放任何密码和密钥：telnet 口令只从环境变量 `ZTE_TN_PASS` 读，Wi-Fi 密钥只写进 0600 临时文件。
