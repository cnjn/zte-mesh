# 测试主机（Ubuntu 编译机 + 小米 AX3600 + ImmortalWrt 虚拟机）

在一台普通 x86_64 Ubuntu 上编译并运行这套代码，不需要 mac、不需要路由器、不需要原厂固件镜像。架构与运行原理见 [ARCHITECTURE.md](ARCHITECTURE.md)，上机流程见 [DEVELOPMENT.md](DEVELOPMENT.md)。
本文只拥有两件事：**第 2.1 节的测试机清单**和**第 4.2 节的 ARM32 模拟器判定表**——别处引用它们，不在别处重复；各套测试的条数基线在 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节，上机待办在 [PLAN](PLAN.md) 第 5 节。

## 1. 能做什么、不能做什么

**能做**：交叉编译全部产物（`arm-linux-gnueabi-gcc` 编 ARM32 soft-float，`aarch64-linux-gnu-gcc` 编路由器侧静态工具）、跑 ARM32 程序（内核对 ARM ELF 走 `binfmt_misc` → `qemu-arm-static`，`F` 标志让它跨 chroot 也有效）、跑服务（`ns-run` 的私有 IPC/网络/挂载命名空间在普通 VM 里正常工作，能完成整个启动序列）、跑主机测试（只有 4 个步骤缺 `out/analysis/cspd`，见第 6 节；各套测试的条数基线见 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节）。

**不能做**：生成可发布的运行包（`make-runtime.py` 会调 `prepare.verify_existing()`，用 `rootfs_sha256` 卡住，合成出来的 `out/` 一定被拒——有意的门槛）、跑 `test_schema` / `test_policies` / `test_profiles` / `test_startup`（要 `out/analysis/cspd`，只有原厂 JFFS2 里有）、上真实路由器联调（OpenWrt 路由器与中兴原厂路由器；没有网桥、没有 1905 对端，也没有 `192.168.5.x`）。

## 2. 一次性准备

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential make git rsync curl ca-certificates file python3 \
  gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi libc6-dev-armel-cross \
  gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user-static binfmt-support
sudo modprobe veth        # ns-run 要求 /sys/module/veth 存在，否则报 private-service-network
cat /proc/sys/fs/binfmt_misc/qemu-arm   # 应有 flags: PF（qemu-user-static 注册 binfmt_misc，ns-run 靠它执行 ARM32，不能省）
# 不需要 Docker：container-make.sh 在这台机器上用不了，一律用 tools/lab/build.sh 代替
```

## 2.1 另外两台：小米 AX3600 与 ImmortalWrt 虚拟机（x86_64）

三台测试机器都在 `192.168.1.0/24`，二层互通（都在同一个 `br-lan` 广播域，1905 帧能互相看见）。这张表是**测试机清单的唯一权威**，[FEATURE_STATUS](FEATURE_STATUS.md)、[DEVELOPMENT](DEVELOPMENT.md)、[INSTALL](INSTALL.md) 都指回这里，不再各写一份机器名单。登测试机一律走 `tools/lab/rssh.py`（密码登录，本机没有 sshpass；**账号与口令只在本机 `.local/lab-env.md`**，已 gitignore，不入库，也不复述进任何文档、日志或命令回显——`rssh.py` 就是读那张表，`ZTE_LAB_USER_<别名>` / `ZTE_LAB_PASS_<别名>` 可单独覆盖）。表里的 `ax3600` / `owrt` / `ubuntu` 是 `rssh.py` 的别名，敲命令时用的就是它，与机型无关。

```sh
export MSYS_NO_PATHCONV=1          # Git Bash 会把 /tmp/x 改写成 Windows 路径
python tools/lab/rssh.py list      # 清单
python tools/lab/rssh.py ax3600 --bench
python tools/lab/rssh.py owrt 'uci show zte_mesh'
python tools/lab/rssh.py ubuntu put out/db-engine-probe /tmp/
```

| 机器（别名 / 机型） | 地址 | 角色 | 关键事实 |
|---|---|---|---|
| `ax3600`：小米 AX3600，刷 ImmortalWrt 25.12.2 | 192.168.1.1 | OpenWrt 路由器（物理机），跑子节点角色 | aarch64 / 内核 6.12.103 / apk 包管理。**没有 `CONFIG_COMPAT`**：ARM32 程序 execve 得 ENOEXEC（shell 报 `syntax error: unexpected word`），也**没有 `BINFMT_MISC`**（`/proc/sys/fs/binfmt_misc` 不存在）；实际跑法是沙箱内那份 `/usr/lib/zte-mesh/root/qemu-arm-static`。**无线只剩一块射频在候选里**（`radio0.disabled=1` 关掉了 2.4G），所以"一个频段有两块射频、`wlan-sync` 放弃该频段"那条路径在这台上测不出来——"没报错"不等于"已验证" |
| `owrt`：ImmortalWrt 虚拟机（x86_64） | 192.168.1.20 | 测试环境里的主控（master） | 内核与 `ax3600` 同一个版本。带启动器的那个包是 `arch:all`（合并前它是单独的 `zte-mesh-launcher`），所以这台装得上——包里带 aarch64 / armv7 / x86_64 各一份构建，`uname -m` 是 `x86_64` 就走进 `arch/x86_64/`（这种架构 2026-11-05 起才有；以前它是 `aarch64_cortex-a53` + `DEPENDS:=@(TARGET_zte\|\|TARGET_qualcommax)`，这台**一个都装不上**，只能 `tools/lab/build-launcher.sh` 手工补）。`root/qemu-arm-static` 仍是手工放的——那是 ARM32 用户态的事，与启动器这种架构无关 |
| `ubuntu`：x86_64 Ubuntu 22.04 | 192.168.1.191 | 编译机 | 本文主角：`gcc-arm-linux-gnueabi` 11.4 + `gcc-aarch64-linux-gnu` 11.4，**不用 Docker**：`make ARMCC=arm-linux-gnueabi-gcc ROUTERCC=aarch64-linux-gnu-gcc database` |

## 3. 日常流程

```sh
# 从 Windows/mac 工作区同步（LAB_HOST 用第 2.1 清单里 ubuntu 的地址；三个环境变量都是必填）
# 它打的是**工作树**，`.gitignore` 对它不起作用——`.local/`（口令表就在这儿）、`out/`、`evidence/` 靠 EXCLUDE 挡
LAB_HOST=192.168.1.191 LAB_USER=<账号> LAB_PASS=<口令> python3 tools/lab/push.py
# 首次或删过 out/ 之后：从运行包重建 out/runtime + manifest.json（见第 5 节）
python3 tools/lab/bootstrap-out.py
# 编译（ROUTERCC=cc 让 ns-run/exec-check 能在本机跑；要打包时才用 aarch64）
sh tools/lab/build.sh all database
sh tools/lab/build.sh ROUTERCC=cc out/ns-run out/ns-run-svctest
# 测试 / 跑服务（需要 root：unshare + bind mount）
sh tools/lab/build.sh test-host
sudo sh tools/lab/run-service.sh --seconds 20
```

远程执行不用装 sshpass：`printf '%s\n' "$LAB_PASS" | python3 tools/lab/rsh.py --stdin 'sudo -S -p "" sh tools/lab/build.sh all'`
——密码只出现在管道里，`ps` 看不到。

### 3.0 重打 runtime 包（probe / ns-run / 随包源码有改动时）

`make-runtime.py` 那边被 `verify_existing()` 卡着（本机 `out/` 是合成的），但**只换成员**的
`openwrt/repack-runtime.py` 不需要 audited `out/`：它保留其余成员逐字节不动，只重算 `SHA256SUMS`。
只有 `root/bin/db-engine-probe`、`ns-run` 这类成员需要交叉编译：

```sh
ARMCC="arm-linux-gnueabi-gcc -D_TIME_BITS=32" sh tools/lab/build.sh database   # probe 是 ARM32 软浮点
ROUTERCC=aarch64-linux-gnu-gcc sh tools/lab/build.sh ns-run                   # ns-run 是路由器的 aarch64
python3 openwrt/repack-runtime.py \
  --replace root/bin/db-engine-probe=out/db-engine-probe ns-run=out/ns-run \
            controller-net.c=src/controller-net.c enroller.c=tools/lab/enroller.c
python3 tests/test_runtime.py                                                 # 守卫里含"成员与仓库文件逐字节相同"（条数基线见 DEVELOPMENT 第 4 节）
```

四个坑：**① `--replace` 是 argparse 的 `store` 语义**，多个替换项必须跟在同一个 `--replace` 后面——写成 `--replace a=b --replace c=d` 只有最后一个生效，包悄悄只换了一个成员。**② 重打的基线**必须是仓库里已提交的那份 tarball：从别处（比如刚把打好的包传回工作区）再往回传，会把基线换成"已经被换过的"那份，成员就退回旧版本了——`test_runtime.py` 的"byte-for-byte"守卫能抓到这件事。**③ 在 Windows 上别用"解出来再 `diff -r`"比包**：归档里 `root/var/tmp`、`root/var/run`、`root/var/log` 是指向 `/tmp` 的符号链接，Git Bash 的 `tar` 建不出链接（没有权限）时会**把链接指向的东西整个拷进解出来的树**——落下的是本机临时目录的一整份**副本**（不是别名，往里写文件本机不会多东西，但删它要递归几 GB 还可能撞 `Device or resource busy`），看着就像"随包带了源码副本"。要比就逐成员比字节（`tarfile` 读两遍归档，比名称/类型/模式/大小/内容哈希，也就是上面第 4 步用的方法），要列成员就用 `tar tvzf`，它会照出 `lrwxrwxrwx`。**④ 换成员还跳过了 `make-runtime.py` 打包前跑的 GNU 符号版本审计**（`vendor_elf.audit()`）：重打完对着成品再跑一遍，不用解包——把归档里 `root/**` 的 ELF 成员读进内存交给它，`missing_strong_symbols` 与 `missing_versions` 都应为 0；`audit()` 按 **basename** 找提供者（`libc.so.6`），照归档成员名（`root/lib/libc.so.6`）建索引的话每个版本需求都会"找不到提供者"，报出几十个假缺符号。

包里的 probe 只要一条性质：不能引用比原厂 glibc 2.26 更新的符号。CI 在 `debian:bookworm` 里编，本机在 Ubuntu 22.04 里编也成立，编完这样自查：

```sh
arm-linux-gnueabi-nm -u out/db-engine-probe | grep -E '__time64|__clock_gettime64|__ioctl_time64|__isoc23_' || echo none
```

### 3.1 agent 模式（M1/M2）

```sh
sudo sh tools/lab/run-service.sh --agent --seconds 120
```

`--agent` 换成 `db-engine-probe --agent-startup`：daemon 以 `Mode=1 / SetRoleEnable=0 / EnMasterOnly=0` 启动，探针这一侧扮演合成主控，走完 Discovery → Topology Query → Search → Search Response → M1 → M2。关注前缀：`ZTE_AGENT_WIRE`（帧收发）、`ZTE_AGENT_WLAN`/`ZTE_AGENT_PSK`（库读回）、`ZTE_TRACE_VALUE`（M2 有没有触发 `cfgChg`、写到哪个 BSS、M2 多长、同步完成没有——四条一次性的证据行）、`ZTE_TRACE_POLL`（daemon 每隔几秒重复的状态行，受下面的采样预算管）。

`--seconds` 要给到 100 以上：一次完整运行约 50 秒，一半是"重启"阶段——两份 M2 落库后探针用 `controller_respawn_daemon()` 把 daemon 换掉（库与订阅池留在探针进程里，等同工厂 `multiapd_mgr` 的 restart），再观察新实例是否重新发 M1。重启后 M1 要十几秒才来（通常两次 topology retry），**不要**因此判断成"它记得自己已配置"。沙箱只给 daemon 一个环境变量（多传一个会让原厂 ld.so 断言），运行时开关一律用 `/run/zte-mesh` 下的 flag 文件（bind mount）：`trace-all` 打开 daemon 日志火管（`ZTE_TRACE_ALL`，上限 2 万行），`mm-off` 关掉 `libzte_agent_mm.so` 的配置管理器替身、让两个 WLAN setter 报告原厂返回值（诊断用），`collect-pause` 只停 `sta-collect` 那一条循环（表留着不再更新时间戳，见 [SERVICE](SERVICE.md)；停 procd 实例会连守护进程一起换 pid，读回来的是重启不是停采集）。

**日志的落点只有一个：logd 那圈环**（两台测试机 `system.@system[0].log_size='128'`，单位 KiB）。服务不写会长大的文件——沙箱根只读、`/tmp` 与 `/var/tmp` 是 8 MiB tmpfs、`/run/zte-mesh` 下全是小文件原子替换——所以 `controller-log.c` 管的是**打进环的比例**，不是轮转。分类与预算：

| 类别 | 预算 | 额度用完／采样时说哪一句 |
|---|---|---|
| 替身自己的调用与返回（ordinary） | 每进程 160 行 | `log_budget=ordinary exhausted=1 limit=160` |
| 1905 / WSC / 绑包等协议行（protocol） | 每进程 64 行 | `log_budget=packet exhausted=1 limit=64` |
| 入网离线（device）/ 配置推送（renew） | 各 48 行 | 不说（量小，静默可接受） |
| `ZTE_TRACE_VALUE` 四条证据行 | 2 万行 | 不采样——判据就靠它 |
| `ZTE_TRACE_POLL` 重复状态行 | **每 60 秒 4 行** | `log_budget=poll sampled=1 limit=4 window=60s dropped_total=N`，每个超额窗口一句 |
| `ZTE_TRACE_ALL` 火管 | 2 万行 | 不说 |

两件事别记反：**展开的是值**，所以带密钥 / SSID / BSS 的格式在 `controller_log_shape()` 里先被否决，永不展开；**每进程的预算重启就重来**（崩溃循环每次只付一小段），**poll 那一路按 `CLOCK_MONOTONIC` 的分钟采样**，进程重启也变不成无界。装机后要看的判据是"推送过去七分钟，`logread` 里还剩不剩用户自己的行"。**`owrt` 那台的 `logread` 会崩**（2026-11-06 装 r23 那趟，`RESULT=PASS` 之后紧跟一行 `Segmentation fault`，`dmesg` 指 `traps: logread[…] general protection fault … in libc.so`；连发五次又全 rc=0，重跑整份验收逐行相同）。**看到崩的是 `logread` 就先看 `dmesg`，别把它读成沙箱里哪个进程死了**——我们的东西崩了会在 `log_error_lines=` 那行露出来，那一趟是 0。`dropped_total=` 是**这个进程开始以来累计丢掉**的 poll 行数：某一分钟丢了多少，看**相邻两句的差**；最后一句之后还在涨，所以它是下界不是总量。**读设备日志先按 pid 切代**——logd 的环不随升级或重启清空，旧一代与新一代的行会同时在环里，混在一起读会得出"采样没生效"的反结论。

`libzte_agent_mm.so` 已在 `spawn_daemon()` 的 `LD_PRELOAD` 里：它只接管 `CmWLANSetAPConfig` / `CmWLANSetPsk`，并且**先把刚写的行读回来比对**，库里已是调用方要的值才把 -3 报成 0（`ZTE_AGENT_MM ... row_applied=1`）。两处读库的坑：`WLANPSK` 必须按实例 id 用 `dbAPIGetView("WLANPSK","DEV.WIFI.AP1.PSK1",…)` 读（密钥在行内偏移 260），用 `dbGetValComm()` 按行号读永远是空值；`CmWLANSetAPConfig` 写库发生在 `cm_wlan_send_cmapi_set_msg` **之前**，所以没有 cspd 时库已经变了、返回值仍是 -3，两者要分开看。

## 4. 与在真实路由器上构建的四处差异，都是环境导致的，不是代码退化

| 差异 | 为什么 | 在哪里 |
|---|---|---|
| `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` | Ubuntu 的 gcc 默认定义它，把 `controller-profile.c:64` 忽略 `write()` 返回值变成 `-Werror` 错误（Debian 不会）；也避免 `memcpy` 走 `__memcpy_chk` | `tools/lab/build.sh` |
| `ROUTERCC=cc` | `ns-run`、`exec-check`、`config-bridge-probe` 是 AArch64 静态程序，x86 上跑不了。**打包前必须用 aarch64 重编** | 命令行参数 |
| `ZTE_MESH_EMULATED_LAB=1` 下的两处仿真 | 只在这个变量存在时生效，真实路由器上的路径不受影响，也不改变权限模型（`ns-run` 本来就要 root 才走到这里）。`shim.c`：qemu 的 `shmat()` 对**固定地址**一律返 `EINVAL`（`NULL` 可以、`fixed`+`SHM_REMAP` 可以、`fixed` 不行），而原厂数据库把段挂在 `0x60000000` 并直接解引用返回值 → 段错误，预载库在 `EINVAL` 时补一次 `SHM_REMAP`。`ns-run.c`：放开 `RLIMIT_AS` 并把它传给 guest（qemu 要预留整个 32 位客户机地址空间 + 翻译缓冲，128 MB 上限必然失败） | `ns-run.c`、`shim.c` |

### 4.1 装在设备上时这两个开关由 init 决定

不需要手工 export：`start_service()` 先看 `uname -m`，arm64 上再判断内核能否原生跑 ARM32；只有确实跑不了、且私有根里存在 `qemu-arm-static` 时才把这两个变量传给 `ns-run`。`zte_mesh.main.emulator` 可指定路径——**私有根内的相对/绝对路径，或宿主上的绝对路径**（后者由 init 比 sha256 后拷进私有根的 `usr/bin/`，见第 4.2 节），`=none` 强制走原生（刷过 `CONFIG_COMPAT=y` 固件之后就是这个状态）。

同一处还问一句启动器：`$NSRUN --launcher-check`，答 127 就不把实例交给 procd。理由与那句拒绝见第 4.3 节。

同一处还有 `zte_mesh.main.al_last`：AL MAC 的末字节（页面"控制器地址末字节"，两位十六进制），默认取 LAN 网桥 MAC 的末字节（`02:00:00:00:19:XX`）；写错格式会记一行日志并改用派生值。两台设备**必须**不同，否则 1905 栈会把对端当自己（[AGENT_PROBE](research/AGENT_PROBE.md) 第 12 节）。

看"线上到底有没有帧"用 `tools/lab/spark.c`：

```sh
zig cc -target x86_64-linux-musl -static -Os -s -o spark-x86_64 tools/lab/spark.c   # 设备上：spark <iface> listen|discovery [秒]
zig cc -target aarch64-linux-musl -static -Os -s -o spark-aarch64 tools/lab/spark.c
```

注意网桥端口的 ingress 帧不会再投给绑定在该端口上的套接字：抓"从线上进来的"用 `tcpdump -i <物理口>`，抓"daemon 发了什么"用 `br-lan`。要看**逐字段**而不是"有没有帧"，用 `tshark` 的 `ieee1905` dissector；沙箱里抓帧的完整步骤（含 `lab-peer` 在宿主命名空间且默认 DOWN、守护进程要有人发 `0x0007` 才会动这两个坑）记在 `WIRESHARK_1905` 第 2 节，那份取证存档不随这一支。

### 4.2 ARM32 模拟器（`qemu-arm-static`）**不入包**

`zte-mesh` 不装它、也不自动下载安装（启动器并进这个包之前，`zte-mesh-launcher` 同样不装）。包里只有原厂 ARM32 运行环境和按 CPU 编出来的启动器；服务能不能起来，取决于设备内核能不能直接跑 ARM32。**这是有意的取舍，不是漏装。**

不进包的四条理由：**OpenWrt 不编它**（feed 里没有 `qemu-user`，打进包就得塞一个预编译第三方静态二进制，二进制再分发的许可与安全维护要另处理）；**必须与宿主架构匹配**（aarch64 设备要 arm64 版、x86_64 设备要 amd64 版，而 `zte-mesh` 是 `arch: all`）——这一条启动器自己用"一个 apk 带三份构建 + 入口脚本按 `uname -m` 选"解决过（第 4.3 节），qemu 不跟着学是因为下面两条；**体积不划算**（静态 qemu-arm 约 11 MB，比整个运行环境包还大，目标设备根本用不到）；**只有一处真的需要**（只有 chroot 里的 `db-engine-probe` 是原厂 ARM32；`ns-run` / `mesh-search` 是按这台机器的 CPU 编的那一份，本来就与模拟器无关）。

init 怎么判定（`zte-mesh.init` 的 `emulator` 段）：

| 情况 | 判定 | 结果 |
|---|---|---|
| `aarch64` / `arm64` | 先读 `/proc/config.gz`（退化 `/proc/config`）的 `CONFIG_COMPAT=y`；**读不到就直接 exec 一次 `$ROOT/lib/ld-linux.so.3`**，能打印自己的 usage（匹配 `*sage:*`）就认定支持 | 支持 → 原生跑 |
| `x86_64` 及其他 | 一律不可能原生 | **必须**有模拟器 |
| 任意 | `zte_mesh.main.emulator=<路径>` | 跳过探测，用指定路径。**两种写法都行**：私有根内的（相对或带 `$ROOT` 前缀的绝对路径），或宿主上的绝对路径（init 比 sha256 后拷进私有根的 `usr/bin/<basename>`，相同就不拷；拷不进去则不起服务并说明原因——它是在 chroot **之后**被 exec 的，沙箱看不见的文件等于没有） |
| 任意 | `emulator=none` | 强制原生。只用于诊断，不是解决办法 |

**"没有答案"不等于"不支持"**：多数固件（ax3600 上的 ImmortalWrt 6.12.103 就是）不导出 `/proc/config.gz`，早期版本因此一律走上模拟器，而包里没有模拟器，服务就每 5 秒退一次——所以探测改成问内核，而不是猜。私有根默认 `/usr/lib/zte-mesh/root`（`ZTE_MESH_ROOT` 可改）；模拟器是在 chroot **之后**被 exec 的，必须放在私有根内部，init 依次找 `/usr/lib/zte-mesh/root/{,usr/bin/,bin/}qemu-arm-static` 三处。找不到会打一条完整说明（含宿主架构与三条出路），随后 `execv errno=8`（`ENOEXEC`）——**看到 `errno=8` 先按这一节处理，不要当成代码 bug**。第 2 节装的 `qemu-user-static` + `binfmt-support` 只方便**在沙箱外**直接跑 ARM 程序；服务是 chroot 后 exec，即便 binfmt 带 `F` 标志能跨 chroot 生效，解释器文件仍须在根内可见。

三条出路，按推荐顺序：

1. **刷带 `CONFIG_COMPAT=y` 的固件**——真实路由器上唯一干净的解法：arm64 内核开了 COMPAT 就能原生跑 ARM32，不需要模拟器，`ZTE_MESH_EMULATED_LAB` 也不会被设置（少了两条仿真专用路径）。配置在 `.github/workflows/build-firmware.yml`（qualcommax/ipq807x 目标，CI 自己编内核，因为 ImageBuilder 改不了内核选项）；qemu 这一层本身可以长期跑，所以它是"要不要拿掉"的可选项而不是故障。
2. **手工放一个模拟器**——测试环境和临时验证用，取**与设备同架构**的一份（编译机上 `tools/lab/emulate.sh` 做的是同一件事；这份文件不在任何仓库里，也不随包分发）。
   取哪一份 deb 与"新版包里只有符号链接"这个坑写在 [README](../README.md) 的「qemu-arm-static 安装指南」里，2026-11-06 逐个下载量过。放好后确认下面两条：

   ```sh
   # 按设备 CPU 取上面 README 表里那一份 deb（24.04 上 apt-get download 取到的就是它，更新的发行版取到的是过渡包）
   curl -O http://ports.ubuntu.com/ubuntu-ports/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_arm64.deb
   dpkg-deb -x qemu-user-static_8.2.2+ds-0ubuntu1.18_*.deb /tmp/q
   ls -l /tmp/q/usr/bin/qemu-arm-static                              # 要看到几 MB 的真文件，不是符号链接
   scp /tmp/q/usr/bin/qemu-arm-static root@<设备>:/usr/lib/zte-mesh/root/qemu-arm-static
   ssh root@<设备> chmod 755 /usr/lib/zte-mesh/root/qemu-arm-static
   ls -l /usr/lib/zte-mesh/root/qemu-arm-static      # 存在且可执行
   service zte-mesh restart; sleep 5
   logread | grep -e qemu -e execv                   # 不该再出现 "no qemu-arm-static was found"
   ```

3. **`emulator=none`**——确认"这块板子到底能不能原生跑"，不是解决办法。

### 4.3 启动器与 CPU：一个 apk 带三份构建，入口脚本按 `uname -m` 选

启动器和运行环境在同一个包里：`zte-mesh` 是 `PKGARCH:=all`（2026-11-05 之前它是单独的 `zte-mesh-launcher`，两部分各一个 apk）。`package-pack.mk` 一个 Makefile 只读一个 `PKGARCH` 写进 `Architecture:`，而 ELF 只认一种机器，所以包里的做法是**按 CPU 各放一份**：

```
/usr/libexec/zte-mesh/ns-run          # 入口脚本（同一份文件装成两个名字）
/usr/libexec/zte-mesh/mesh-search     #   —— $0 的最后一截决定跑哪个程序
/usr/libexec/zte-mesh/arch/aarch64/{ns-run,mesh-search}
/usr/libexec/zte-mesh/arch/armv7/{ns-run,mesh-search}
/usr/libexec/zte-mesh/arch/x86_64/{ns-run,mesh-search}
```

`uname -m` 决定进哪个目录（`arm64`→`aarch64`、`armv7l`→`armv7`），选中就 `exec`——换程序不换 pid，procd 盯的就是那个 pid，`attach` 的 `sandbox_pid()` 也还在 `/proc/*/cmdline` 里 grep 它。

**代价是装机时不再挑机器**：以前的 `DEPENDS:=@(TARGET_zte||TARGET_qualcommax)` 会在 `apk add` 上失败（apk 求解全有或全无，一台不匹配的机器会让同一条命令里的其他包一起装不上），现在什么机器都收，那句拒绝改由入口脚本在运行时说清（报本机 `uname -m` 与包里带的几种架构，`exit 127`），`zte-mesh.init` 在把实例交给 procd 之前先问一次 `--launcher-check`——否则 `respawn 3600 5 0` 会每 5 秒重开一个 127，而日志里从头到尾不出现"架构"两个字。**问的就是装机要跑的同一份代码**：CPU→目录那张映射表不在 init 里抄第二份，读回与人手工跑看到的是同一句第一行。**只有 127 才是"这种架构不在包里"**：其余非零（例如 `/usr/libexec/zte-mesh/ns-run` 站着的是合并前手工放的旧 ELF，它不认识这个参数、只打 usage 并回 64）由 init 另记一行 `--launcher-check exited <码>`，不冒充架构判定。

构建侧：`tools/build/launcher-arches.sh SRC OUT` 每种架构一次 `-static`（[DEVELOPMENT](DEVELOPMENT.md) 第 8 节；`zte-mesh` 的 `Build/Compile` 从运行包解出三个 `.c` 再调它）。`$(TARGET_CC)` 只保本目标那种架构，脚本用 `-dumpmachine` 认它属于哪个目录名——不认 `ARCH_PACKAGES` 这类内部变量，因为目录名必须和产出它的那台编译器一致。其余两种架构要宿主的交叉编译器（CI 那一步装 `gcc-aarch64-linux-gnu` + `gcc-arm-linux-gnueabihf`；本机 `ubuntu` 那台两种架构都有，见第 2.1 节）。可用的环境变量：`ZTE_MESH_ARCHES`（编哪几种架构）、`ZTE_MESH_REQUIRE_ARCHES`（缺了哪种架构就报错，CI 设成三种架构全要，于是"少编一种架构"从绿日志里的一行变成红）、`ZTE_MESH_CC_<档>`（指定编译器）。armv7 先试 hard-float、退一步用 soft-float：`-static` 的程序自带 libc，浮点 ABI 在运行时不是任何人的事。

测试环境上这三条各判什么：

| 要做的 | 命令 | 看什么 |
|---|---|---|
| 这台机器的 CPU 在不在包里 | `/usr/libexec/zte-mesh/ns-run --launcher-check` | 退出码 0 且打印 `…/arch/<这种架构>/ns-run`；非 0 时那一句会带上 `carries:` 后面包里的目录名 |
| 包里的哪几种架构 | `ls /usr/libexec/zte-mesh/arch/` | feed 构建树里只带目标机那种架构，属正常；CI 出的 apk 三个目录都在 |
| 这种架构不在包里，非要这台跑 | `tools/lab/build-launcher.sh`（原生 gcc 编一份） | 它现在把二进制放进 `arch/<cpu>/`（目录名按上面那张映射，脚本自己读目标机的 `uname -m`）——**放到 `/usr/libexec/zte-mesh/` 那一层没用**：那两个名字属于包，`apk add` 会重写，而入口只往 `arch/` 下面找 |

**armv7 那种架构目前没有测试机**（三台测试环境的 CPU 是 aarch64 / x86_64 / x86_64，中兴原厂路由器不吃 apk），所以那种架构只有编译与打包级的证据，运行时读回来的是 [PLAN](PLAN.md) 的 H-16。ax3600 跑沙箱要靠私有根里那份 `qemu-arm-static`——那是 ARM32 用户态的事，与 `arch/aarch64/` 这两个二进制无关（它们本来就是 aarch64 机器码）。


## 5. 从运行包重建 `out/`

`tools/dev/prepare.py` 是 `out/` 的权威来源，但它要求原厂 `rootfs.jffs2`（哈希固定）。没有镜像时用 `tools/lab/bootstrap-out.py`：解开 `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` 并逐个核对 `SHA256SUMS` → 核对 `multiapd` 与 `prepare.MULTIAPD_SHA256` 一致（`a0ba6bee…`）→ 用 `vendor_elf` 走一遍 `DT_NEEDED` 闭包（27 个库）+ 符号审计 → 按它的权限规则写出 `out/runtime` 与带 `"source": "runtime-bundle"` 的合成 `manifest.json`。合成清单的 `rootfs_sha256` 为空，`prepare.verify_existing()` 会拒绝它，打包流程不可能误用；想让 `make-runtime.py` 通过的唯一办法是拿到原厂镜像跑真正的 `prepare.py`。**注意顺序**：`bootstrap-out.py` 会重建 `out/runtime`，把已编好的 `libzte_*.so` 一起清掉，之后再跑一次 `sh tools/lab/build.sh all database`（`mkroot.sh` 缺 shim 会直接报错）。

## 6. 这台机器上的测试状态与还缺什么

**通过**：`controller-wsc-codec`、`controller-packet-codec`、`cm-profile-codec`、`bridge-codec`、`test_evidence.py`、`test_device_workspace.py`、`test_preload.py`、`test_sandbox.py`（沙箱真的 unshare 并 chroot 进 ARM32 root）、`test_init.sh`、`test_rpcd.sh`（shell 测试要先转 LF，`push.py` 已处理），以及 `tools/dev/cm-profile-evidence.py --verify src/cm-profile-layout.inc`（44 处 payload、2496 字节，与 `libcmapi.so`/`multiapd` 的实际布局一致）。**失败**：`test_schema`、`test_policies`、`test_profiles`、`test_startup`——全是同一个 `FileNotFoundError: out/analysis/cspd`，唯一补法是把原厂镜像放到 VM 上再 `python3 tools/dev/prepare.py --rootfs <镜像> --analysis-cspd`；没有镜像时它们只影响 `cspd` 证据，不影响别的。还缺一条：`WlanCfgDbWrite` 在 `db-engine-probe` 的库上是否成功，**未验**（[AGENT_PROBE](research/AGENT_PROBE.md) 第 6 节的运行时那部分；工具是 `shim.c` 记录全部 `CmWLAN*` 调用、`emulate.sh --strace` 跟踪系统调用）。**各套测试的条数基线只写在 [DEVELOPMENT](DEVELOPMENT.md) 第 4 节**，这里不维护数字；上面标"未验"的，不要在别的文档里写成已验，排期看 [PLAN](PLAN.md) 第 5 节。

## 7. 踩过的坑

| 现象 | 原因和解法 |
|---|---|
| `arm-binfmt-P: Could not open '/lib/ld-linux.so.3'` | 直接执行 ARM 程序时 binfmt 按绝对路径找解释器。改为显式给 loader：`out/runtime/lib/ld-linux.so.3 --library-path … <程序>`；`ns-run` 是先 chroot 再 exec，没这个问题 |
| `ZTE_DB_ENGINE fail=not-launched-by-sandbox` | 正常：`db-engine-probe` 只接受 `ns-run` 的启动方式 |
| `ZTE_DB_SANDBOX error=private-service-network errno=2` | 没加载 `veth` 模块。`sudo modprobe veth` |
| `arm-binfmt-P: allocate 134217728 bytes for jit buffer` | `RLIMIT_AS=128 MB` 装不下 qemu。设 `ZTE_MESH_EMULATED_LAB=1`（`run-service.sh` 已设） |
| `ZTE_DB_ENGINE fail=namespace-not-empty` | 在共享 IPC 命名空间里跑，库里有残留段。只能用沙箱 |
| `qemu: uncaught target signal 11` + 日志里有 `emulated=shmat-fixed-address-retry` | 正常，重试成功后继续。没有那一行仍段错误就看 `emulate.sh --strace` 的最后几条系统调用 |
| `trap: EXIT: bad trap` / `case … in` 语法错误 | 脚本被 CRLF 污染。`push.py` 现在会转 LF |
| `wrong ELF class: ELFCLASS32`（LD_PRELOAD） | 只在沙箱外用 binfmt 跑时出现（宿主 ld.so 看到 ARM 库），无害 |
| `cannot be preloaded` 之后出现 `ZTE_MESH_SHIM loaded` | shim 在 `root/lib` 里但 `out/runtime/lib` 已被 bootstrap 清掉。重跑 `build.sh all` |
| `chroot: failed to run command '/bin/sh'` | 手工搭沙箱时指向了宿主 `/bin/sh`；沙箱根里只有原厂二进制，用 `chroot ROOT ./bin/...` |
| `ash: -c requires an argument`（远端什么都没执行） | **`rssh.py` 没有 `--sh` 这个选项**：主机名之后以 `--` 开头的参数会被当成命令的一部分发给远端，于是远端收到 `--sh ls -l /tmp`（选项与命令现在手动切分）。要把管道和 `&&` 当一条远端命令送，就**整个用一个引号参数**（`rssh.py owrt 'a \| head'`）。反过来说：本地每个参数是被 `" ".join` 拼回去的，所以**引号会丢**——`rssh.py owrt grep "a b" f` 到了远端是 `grep a b f`（三个参数），带空格的匹配串要在本地那一层就写成远端语法的引号，而不是靠 shell 的引号 |
| `echo HI` 返回 **127** | dropbear 带 pty 的 exec 会把命令当成登录 shell 的输入。`rssh.py` 默认不开 pty，需要时用 `--pty` |
| `open_sftp()` 抛 `EOF during negotiation` | AX3600 的固件没有 `openssh-sftp-server`。`rssh.py` 自动退回 `cat` 管道（二进制安全） |
| 本机 `scp`/参数里的 `/tmp/x` 变成 `C:/Users/.../Temp/x` | **Git Bash 的路径转换**。调用前 `export MSYS_NO_PATHCONV=1`（`MSYS2_ARG_CONV_EXCL='*'` 亦可） |
| 构建机"能编"却被判定等于仓库 | 覆盖同步（`tar -xzf ... -C ~/zte-mesh`）**不删远端多余文件**：仓库里删掉的文档、临时目录（`cmp/`、`dist/`、根目录那些旧的 `.c`）会一直留着。判"这台机器等于仓库"要**比文件清单**（`git ls-files` 对 `find`），不是看 `make` 过不过；同理 `make` 只比 mtime，陈旧产物会混进哈希比对（见 [产物哈希基线](DEVELOPMENT.md) 第 4 节那条） |
| `ash: syntax error: unexpected word (expecting ")")` 执行 ARM32 程序时；或 `mount: mounting none on /proc/sys/fs/binfmt_misc failed: No such file or directory` | 内核没有 `CONFIG_COMPAT`，execve 返回 ENOEXEC，shell 把二进制当脚本读（AX3600 原厂固件就是这样）；同一条的另一面是内核也没编 `BINFMT_MISC`，透明执行这条路直接没有。两条出路见 `BLOB_SHIM` 的 qemu 分支和 `build-firmware.yml`，判定表见第 4.2 节 |
| `error: ignoring return value of 'write' ... -Werror=unused-result` | `(void)write(...)` 从 GCC 9 起不算"用了返回值"。用 `emit()` 辅助函数真赋值再丢弃，别写 `(void)write` |
| 装上的 `ns-run` 执行报 `not found`，`ls` 明明在 | 编译机 glibc、目标机 musl，动态链接的 ELF 找不到 `/lib64/ld-linux-x86-64.so.2`，`execve` 返回 ENOENT，shell 就报 `not found`。**加 `-static`**，别被 "not found" 骗成路径或权限问题。包里那三份构建全部 `-static`（`tools/build/launcher-arches.sh` 把它写死在 `FLAGS` 里，调用方拿不掉），理由就是这一条：apk 装到哪台机器都不保证那台机器的 libc 与编译器同一个 release |
| 覆盖正在运行的 `ns-run` 得到 `Text file busy` | 先 `/etc/init.d/zte-mesh stop` 再换文件，换完再 start。**2026-11-05 起还有一层**：`/usr/libexec/zte-mesh/{ns-run,mesh-search}` 是包里的 shell 入口，真正的二进制在 `arch/<cpu>/` 下面——手工换文件要换 `arch/` 里那一份；换成外层那两个名字，等于把入口脚本顶掉，而 `zte-mesh.init` 正拿它问 `--launcher-check`（真二进制不认这个参数、返回 64，于是看起来像"这台机器不在包里"） |
| `apk info -e` 在设备上回 **0 行**，明明装着包 | 那台机器的 `apk-tools 3.0.5` 就是这样（两台测试机 `owrt` / `ax3600` 实测各回 0 行），不是"这台没装"。数装了什么用 `apk list --installed | grep zte`；`apk info -L <包>` 看一个包声称拥有哪些文件（判叠加态里"入口归谁"用的就是它） |
