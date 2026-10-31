# 开发、构建、测试与真机联调

架构见 [ARCHITECTURE.md](ARCHITECTURE.md)，服务配置语法见 [REAL_CHILD.md](REAL_CHILD.md) 第 3 节，脚本见 [tools/README.md](../tools/README.md)。

## 1. 机器与连接

| 角色 | 怎么连 | 说明 |
|---|---|---|
| mac（构建机） | `ssh 192.168.167.93` | 主要工作副本 `/Volumes/code/sr1010-mesh`。编译在离线 Docker 镜像 `sr1010-mesh-shim-builder` 里做；`out/`、`evidence/` 只在这里 |
| 开发机 | — | 克隆后 `git push`；mac 仓库设了 `receive.denyCurrentBranch updateInstead`，推送直接更新工作区 |
| SR1010 路由器 | 从 mac：`ssh root@192.168.5.1` | 实验室脚本默认 `--host root@192.168.5.1 --port 22`。没有 python3、没有 `pkill`，只有 BusyBox 和 `tcpdump` |
| 真子路由 ZXHN Z506 | 从 mac：telnet `192.168.5.216:23`（`tools/child/ztetn.py` 默认值，可用 `--host`/`CHILD=` 改） | 口令只从 `ZTE_TN_PASS` 读（用户名 `ZTE_TN_USER`，默认 root），**不要写进任何文件** |
| OpenWrt 构建机 | `ssh -p 2022 192.168.223.19`，树 `~/workspace/immortalwrt` | 只用来编包（第 8 节） |
| 实验室虚拟机 | `ssh zzb@10.23.90.131`（x86_64 Ubuntu 22.04） | 没 mac、没路由器、没原厂镜像时也能编译、跑主机测试（`tools/build/bootstrap-out.py` 从运行包重建 `out/`） |

## 2. 构建

在 mac 上（`orbctl start` 先把 Docker 拉起来）：

| 命令 | 做什么 |
|---|---|
| `sh build.sh` | 完整流程：从固件提取并审计运行环境（`prepare.py`，需要 `unsquashfs`）、核对表结构证据、建镜像、`make all database controller test-host`、离线加载测试 |
| `sh tools/build/container-make.sh <目标>...` | 在容器里编指定产物（`db-engine-probe`、`zte-meshctl`、`ns-run`、`libzte_controller_trace.so` 等）或跑 `test-host`、`controller`。会在容器里先删旧产物，保留 make 的退出码 |
| `sh tools/build/runtime.sh svctest` | 实验室运行包 `out/zte-mesh-svctest.tar.gz`（启动器根是 `/tmp/zte-mesh-svc/root`，路由器上只写 tmpfs） |
| `sh tools/build/runtime.sh install` | 包用的运行包 `openwrt/zte-mesh/zte-mesh-runtime.tar.gz`（随仓库提交，根是 `/usr/lib/zte-mesh/root`） |

- 原厂镜像默认读 `../zx279133/vendor-reference/2b5/rootfs.jffs2`，`SR1010_VENDOR_ROOTFS` 可改。重复准备会核对已有文件哈希。
- **在 mac 上不要原地覆盖源码文件再编译**：容器经 virtiofs 读源码，变短的文件报 `is shorter than expected`，变长的可能编出
  新旧混合的东西。用 git 更新，或先写临时名再 `mv`。产物同理，所以 `container-make.sh` 在容器里删。
- 运行包内有 `SHA256SUMS` 自校验，`make-runtime.py` 还会审计 ARM32 ABI（强符号、GNU 符号版本）。
- x86_64 主机可用 `Dockerfile.cross`（`make ROUTERCC=aarch64-linux-gnu-gcc`）交叉编译。

## 3. 跑控制器

三种方式，都在 mac 上执行：

1. **实验室（不碰子节点）**：`python3 dev-service-smoke.py --no-attach --seconds 24`——路由器私有命名空间里跑服务，
   验证配置、热更新、守护进程重启，不把线接进 `br-lan`。`dev-config-check.py DIR` 批量喂配置看接受/拒绝，
   `dev-sched-check.py` 用真实时钟测定时窗口和访客限时（约 5 分钟）。
2. **真子节点**：`python3 tools/capture/mreplay.py --config <0600 配置> --expect-macs <允许的 MAC> --out <证据目录> --seconds N`。
   接进 `br-lan`，触发子节点重新入网（默认把子节点端口 down/up），抓两侧的包、服务日志和 `state.json`；`--swap 秒:文件` 中途换配置，
   `--action '秒:reboot <mac>'` 或 `'秒:light <mac> 0|1'` 发节点动作，`--al-last` 每次换一个控制器 AL MAC。
3. **已安装的包**：`uci set zte_mesh.main.enabled=1` 等，`/etc/init.d/zte-mesh start`；状态 `ubus call luci.zte-mesh status`。
   只验证 procd/rpcd 而不碰子节点时，建一个空网桥（`ip link add br-zmtest type bridge`），把 `zte_mesh.main.bridge` 指过去。

- 配置文件里有 Wi-Fi 密钥：放仓库外、0600、跑完删除（证据目录里也不要留）。
- 服务运行目录：配置 `/run/zte-mesh/config`、状态 `state.json`、节点动作 `actions/`、子节点摘要 `peers/`。
- 调试日志级别（原厂守护进程）：`sendcmd multiapd.acd.acd -l 8`（还有 `base`、`topo`、`slave`），重启后复位，结束用 `-l 7` 还原。

## 4. 测试

- 主机测试全在容器里：`sh tools/build/container-make.sh test-host`（C 编解码含 10 万次变异、证据处理、表结构、策略、profile、
  启动视图、init 79 条、rpcd 15 条、preload、沙箱）。看到 `make: *** ... Error` 就是失败，退出码非 0。
- LuCI 页面的 JS 测试：`node tests/test_luci.js`（模拟 LuCI 的 `form`/`uci`/`rpc`；mac 上没有 node，在开发机上跑）。
- 单个 Python 测试按脚本方式跑，第一个参数是产物目录：`python3 tests/test_schema.py out`（`unittest discover` 不适用）。
- 不依赖 `out/` 的三组可以直接在本机跑：`sh tests/test_init.sh`、`python tests/test_runtime.py`、`python tests/test_i18n.py`。
- 容器把 `/tmp` 挂成 noexec：测试不能执行自己写到 `/tmp` 的脚本（`tests/test_rpcd.sh` 用 shell 函数和 `sh -c`），
  dash 的 `.` 不接受额外参数（先 `set --`）。

## 5. 真机测试守则

向真机推配置会真实修改并持久化它的设置。

- **每轮前后读回子节点基线**：`ZTE_TN_PASS=… sh tools/child/childread.sh`，8 行 `WLANCfg` 与 [REAL_CHILD.md](REAL_CHILD.md)
  末尾的基线逐行比较。改槽位的实验先在实验室验证（`device-controller-smoke.py` 的 `map_teardown` 必须为 0），真机轮询
  子节点 `WLANCfg`，一出异常立刻用"全部槽位 + 一次 Renew"恢复。
- **永远不要向真机推占位值。** 额外 BSS 的 `enabled/ssid/key` 必须三项给齐；同一无线电的槽位必须是 1..k。
- **不要改子路由的模式**（`Mode`、自组网 V2、`VersionMode`、MESH 开关），这些由用户在子路由上设置。
- **接入保护**：网桥转发表里出现不在 `--expect-macs` 里的 MAC 就拒绝接入。长期放行：控制器 `d0:11:e5:c5:cb:4a`、
  子路由 `d4:b7:09:0d:77:30`、一加 `32:83:90:d9:0f:62`、iPhone `aa:8b:bc:9f:1f:c7`、Mac `aa:c9:63:3e:7e:70`、
  用户电脑 `e8:bf:b8:9b:40:a1`。新 MAC 先用 `/tmp/dhcp.leases` 确认是普通客户机，不确定就问用户（一加还会用私有 MAC
  `42:be:ab:fd:c3:88`，不在长期名单里）。
- 会让用户设备掉线的测试（关主 SSID、ACL 封禁真实终端、信道/功率、改 SSID、定时 `scope=all`）必须先得到用户同意，
  测完立即还原。用户的电脑可能就接在子节点后面。
- 还原办法：信道先发原信道再发 `0`（自动）；SSID 被重配后掉到隐藏默认名时改成别的名字再改回；ACL 删光所有 `acl.N.*`；
  功率设回最大档。子路由原始值备份在 `evidence/build-mesh/config_z506.xml`（含明文密钥，不外传）。
- 不要在局域网里有别的真实 mesh 节点时把沙箱线接到 `br-lan` 用测试配置。

## 6. 踩过的坑

- **守护进程环境里只能有一个变量**（`ZTE_MESH_START_COUNT`）：多一个 `setenv` 原厂 ld.so 就断言失败
  （`dl-minimal.c: 126: realloc`，exit 127）。给 preload 库传状态走数据库。
- **路由器没有 `pkill`**：脚本按命令行找进程再杀（`mreplay.py` 曾每轮漏两个 `tcpdump`，攒到 66 个占 130 MB）。每轮后看 `ps`。
- **不要用 `kill multiapd` 逼子路由重新入网**：反复这样做后子路由日志出现 `restart unexpected N times`、
  `SubScribPublish ... already exist`，`cspd` 收不到上线通知（FINDINGS 3.4）。改用重启控制器、`zte-meshctl renew-acd <band>`、
  `mreplay.py` 的端口 down/up，或重启子路由。
- **不要自己发 ACD 事件 2**：守护进程收到第一个 1905 帧时自己发，提前发会用掉一次性的 `g_AcdsRenewStat`。
- **`pkill -f "<模式>"` 会杀掉自己**（命令行里含同样字符串的 shell 也匹配）：用 `[.]` 之类的写法。
- **zsh**：变量不自动分词；以 `=====` 开头的词会被当成命令展开。
- **子路由 telnet 会话不能叠加**；两个 `sendcmd` 同时跑会串话。重启会清掉 `/var/tmp/busybox`，自带工具只有
  `grep tr cut sed awk pgrep wget tftp`；行缓冲超过约 1 KB 会截断，长内容按 500 字符分块 base64 发（`tools/capture/vt.sh`）。
- `iwconfig` 在刚重启或已关闭的接口上读回的 SSID 不可靠；短时间连续重配 2.4G 会掉到隐藏默认 SSID。
- 测试路由器没有授时，时钟可能差十几个小时：定时窗口按路由器 `date -u` 算。
- **装包后服务起不来，先看是不是缺模拟器**：日志里 `execv errno=8` 几乎总是这个（`ENOEXEC`，内核拒绝执行 ARM32 ELF），
  不是代码退化。确认顺序：`uname -m` → 私有根里有没有 `qemu-arm-static` → 第 11 节。

## 7. 改了控制器代码之后的检查

1. 开发机提交并推送 → mac 上 `container-make.sh` 编改动的产物（零警告）→ `test-host` 通过 → `runtime.sh svctest`。
2. 实验室：`dev-service-smoke.py --no-attach`；涉及配置解析的跑 `dev-config-check.py`；涉及调度的跑 `dev-sched-check.py`。
3. 真子节点（需要时）：`childread.sh` 基线 → `mreplay.py` → 看 `state.json` 节点 `model:"ZXHN Z506"`、`zte:true`、
   `online:true` → `childread.sh` 读回。
4. 结束：路由器无残留（`/run/zte-mesh`、`/tmp/zte-mesh-svc`、`zmesh0`、`ns-run`、`db-engine-probe`、`tcpdump`）；
   mac 上含密钥的配置已删。

## 8. OpenWrt 包、版本控制与 CI

**feed**：本仓库是 ImmortalWrt 的一个 feed，包在 `openwrt/` 下。构建机的树（25.12，目标 `zte/zx279133`，apk 包管理，
分支 `sr1010-zx279133`）里登记两处：`feeds.conf.default`（已提交，`src-git zte_mesh https://github.com/cnjn/zte-mesh.git`，
不钉版本，每次 `feeds update` 取最新 `main`）和 `feeds.conf`（不受 git 跟踪、存在时优先，`src-link zte_mesh <本地克隆>/openwrt`）。

**打包**：

```sh
# mac：重新生成运行包（含原厂二进制，随 feed 提交）
sh tools/build/runtime.sh install                       # -> openwrt/zte-mesh/zte-mesh-runtime.tar.gz，提交它
# 构建机
git -C ~/workspace/zte-mesh pull && cd ~/workspace/immortalwrt
./scripts/feeds update zte_mesh && ./scripts/feeds install -a -p zte_mesh
# .config 里加 CONFIG_PACKAGE_{zte-mesh,zte-mesh-launcher,luci-app-zte-mesh,luci-i18n-zte-mesh-zh-cn}=m，再 make defconfig
make package/feeds/zte_mesh/zte-mesh/compile package/feeds/zte_mesh/zte-mesh-launcher/compile \
     package/feeds/zte_mesh/luci-app-zte-mesh/compile \
     package/feeds/zte_mesh/luci-i18n-zte-mesh-zh-cn/compile   # 翻译是独立的包，不编译它就没有中文
```

- 包直接解包同目录下提交的 `zte-mesh-runtime.tar.gz`，不需要往 `dl/` 放东西；`ZTE_MESH_RUNTIME=` 可指向别的运行包。
- **两个包目录是必需的**：`package-pack.mk` 的 `Architecture` 读的是 Makefile 级全局 `PKGARCH`，一个 Makefile 给不了两个架构。
  `zte-mesh` = `arch: all`（脚本、LuCI、rpcd、配置 + 运行时 tarball），`zte-mesh-launcher` = `aarch64_cortex-a53`
  （用 `$(TARGET_CC)` 现编 `ns-run` 与 `mesh-search`）。数据包必须显式写空 `Build/Compile`，否则 OpenWrt 会在只有 tarball 的
  构建目录里跑 `$(MAKE)` 报 `No targets specified and no makefile found`。
- 大小：`zte-mesh` 4.1 MB（装上占 jffs2 约 4.9 MB，解压约 8 MB，大头是原厂库），LuCI 与中文包合计约 11 KB。
- 包没签名：`apk add --allow-untrusted`；卸载 `apk del <四个包名>`。运行包含原厂二进制，随这个公开仓库一起发布
  （2026-10-19 决定）；`zte-mesh` 仍是 `PKG_FLAGS:=nonshared`，不进共享软件源。
- **包里没有 ARM32 模拟器**（2026-10-31 定）：x86_64 设备和没开 `CONFIG_COMPAT` 的 arm64 设备必须自己把 `qemu-arm-static`
  放进私有根，否则 controller 实例每 5 秒以 `execv errno=8` 退出。理由与三条出路见第 11 节。

**CI 全部手动触发**（`workflow_dispatch`），push 和 PR 都不会自动跑构建：

| workflow | 产物 | 说明 |
|---|---|---|
| `build-apk.yml` | `zte-mesh-apk`（四个 apk） | `ubuntu-latest` 交叉编译 `zte/zx279133`，断言四个包都在。产物**未签名**，装的时候要 `--allow-untrusted` |
| `build-firmware.yml` | `CONFIG_COMPAT=y` 的固件 | 真机摆脱模拟器的唯一干净解法 |
| `build-lab-tools.yml` | `lab-tools`（ARM32 宿主件与构建日志） | 见第 10 节。只上传 artifact，**不往仓库提交任何东西** |

**版本控制**：仓库在 GitHub `cnjn/zte-mesh`，只有 `main` 一条分支。`out/`、`evidence/`、pcap、`*.i64`、apk、tar.gz
在 `.gitignore` 里，提交前确认没有原厂二进制、证据或密钥混进来。

## 9. 界面文案与中文翻译

界面上**所有**给用户看的文字都要走 `_()`，msgid 就是那句英文本身（LuCI 用 msgid 当键，没有别名字符串）。改文案时
`.po` 里那条旧 msgid 就失效了——它不会报错，只会安静地变回英文，所以有一条测试盯着这件事。

```sh
python tests/test_i18n.py        # 12 项：覆盖率、格式符、菜单标题、编译成 lmo 后逐条能查到
```

测试做四件事：视图里每个 `_()` 都在 `.po` 里有译文；`.po` 里没有源码已不再用的条目；`%s`/`%d` 在译文里个数与顺序一致；
编成 lmo 后每条 msgid 都能按哈希查到译文。加新文案的流程：写 `_('...')` → 把同一句英文抄进
`openwrt/luci-app-zte-mesh/po/zh_Hans/zte-mesh.po` 并翻译 → 跑上面的测试。

**它怎么到设备上**：`luci.mk` 把 `po/<lang>/` 编成独立的包 `luci-i18n-<app>-<lang>`，文件落到
`/usr/lib/lua/luci/i18n/zte-mesh.zh-cn.lmo`；`zh_Hans` 经 `LUCI_LC_ALIAS` 映射成 `zh-cn`。装应用包**不会**带上它——
`build-apk.yml` 因此单独编译它，并断言四个 apk 都在。页面从 `/cgi-bin/luci/admin/translations/<lang>` 拿到所有
`<lang>.lmo` 拼成的 JS，`cbi.js` 的 `_()` 查的就是它——所以"源码里有译文"和"设备上有 lmo"是两件事，缺一件页面就是英文。

手工装到运行中的设备（实验室的路由器是拷文件而不是装包）：

```sh
python tools/build/po2lmo.py openwrt/luci-app-zte-mesh/po/zh_Hans/zte-mesh.po /tmp/zte-mesh.zh-cn.lmo
cp /tmp/zte-mesh.zh-cn.lmo /usr/lib/lua/luci/i18n/     # 不需要重启服务，刷新页面即可（Ctrl+Shift+R 绕过缓存）
```

`tools/build/po2lmo.py` 不是另写一套格式：按 `modules/luci-base/src/po2lmo.c` 与 `lib/lmo.c` 实现（字符串块 + 16 字节索引 +
结尾块长度，键用 SuperFastHash 且以长度作种子），只用来给没有构建机的设备补装；真正的包仍由构建树的 `po2lmo` 生成。

语言怎么选：LuCI「系统 → 语言和界面」里选简体中文（写进 `luci.main.lang`），或保持 `auto` 让 `Accept-Language` 决定。
中文目录只翻这一页。

## 10. ARM32 宿主件（`build-lab-tools.yml`）

仓库里**不放任何构建产物**（`/prebuilt/` 在 `.gitignore` 里）。`build-lab-tools.yml` 手动触发，产物只作为 Actions
artifact `lab-tools` 发布（构建失败时也上传，构建日志 `*-build.log` 在里面）：

| 文件 | 是什么 | 谁用 |
|---|---|---|
| `db-engine-probe` | ARM32、`-nostdlib` 链原厂 glibc 2.26 的沙箱宿主程序 | **产线**：`repack-runtime.py --replace` 把它塞进 `zte-mesh-runtime.tar.gz` 的 `root/bin/`，随 apk 分发 |
| `libzte_mesh_shim.so` / `libzte_agent_mm.so` | ARM32 的原厂符号 shim | 同上，按需重新打进 tarball |
| `ns-run-x86_64` / `ns-run-aarch64` | 两份宿主架构的沙箱启动器 | 只用来验证能交叉编译；包里的 `ns-run` 由 launcher 现编 |
| `*.sha256`、`*-build.log` | 校验和与构建日志 | 追溯 |

**它为什么存在**：`db-engine-probe` 是 ARM32、`-nostdlib`、要对准原厂 glibc 2.26，**在 mac/Windows/路由器上都编不了**，
只能在 CI 的 `debian:bookworm` 容器里用 `arm-linux-gnueabi-` 交叉编。流通路径是
`build-lab-tools.yml`（容器编）→ 下载 artifact → `repack-runtime.py --replace` → tarball（提交）→ apk。
`build-apk.yml` 不碰这些产物，它只通过 tarball 间接影响 ARM32 那一半。

**怎么更新**：手动跑 `Build lab tools`，下载 `lab-tools` artifact 解到 `prebuilt/`（已忽略，不会被提交），然后
`python3 openwrt/repack-runtime.py --replace root/bin/db-engine-probe=prebuilt/db-engine-probe`（参数见脚本头部注释）
重新生成 tarball 并提交。漏了这步就会被 `tests/test_runtime.py` 拦下——它断言 tarball 里那份必须带 `config-reject` 与
`wlan-plan` 两个标记，这两个正是 C6 那次"冻结的副本不认 `mesh.role=slave`"的特征串。

## 11. ARM32 模拟器与仿真开关

下面几处差异都由仿真环境导致，不是代码退化：

| 差异 | 为什么 | 在哪里 |
|---|---|---|
| `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` | Ubuntu 的 gcc 默认定义它，把 `controller-profile.c:64` 忽略 `write()` 返回值变成 `-Werror` 错误（Debian 不会）；也避免 `memcpy` 走 `__memcpy_chk` | 实验室虚拟机的编译参数 |
| `ROUTERCC=cc` | `ns-run`、`exec-check`、`config-bridge-probe` 是 AArch64 静态程序，x86 上跑不了。**打包前必须用 aarch64 重编** | 命令行参数 |
| `ZTE_MESH_EMULATED_LAB=1` | 见下两条 | `ns-run.c`、`shim.c` |
| `shim.c` 的 `shmat` 重试 | qemu 的 `shmat()` 对**固定地址**一律返 `EINVAL`（`NULL` 可以、`fixed|SHM_REMAP` 可以、`fixed` 不行）。原厂数据库把段挂在 `0x60000000` 并直接解引用返回值 → 段错误。预载库在 `EINVAL` 时补一次 `SHM_REMAP` | `shim.c` |

两条代码改动都只在 `ZTE_MESH_EMULATED_LAB` 存在时生效，真机路径不受影响：`ns-run.c` 放开 `RLIMIT_AS`（qemu 要预留整个
32 位客户机地址空间 + 翻译缓冲，128 MB 上限必然失败）并把它传给 guest；`shim.c` 在 `shmat` 失败且 `errno == EINVAL` 时带
`SHM_REMAP` 重试一次。两者都不改变权限模型（`ns-run` 本来就要 root 才走到这里）。

### 11.1 装在设备上时这两个开关由 init 决定

不需要手工 export：`start_service()` 先看 `uname -m`，arm64 上再判断内核能否原生跑 ARM32；只有确实跑不了、
且私有根里存在 `qemu-arm-static` 时才把这两个变量传给 `ns-run`。`zte_mesh.main.emulator` 可指定路径，
`=none` 强制走原生（刷过 `CONFIG_COMPAT=y` 固件之后就是这个状态）。

同一处还有 `zte_mesh.main.al_last`：AL MAC 的末字节，默认取 LAN 网桥 MAC 的末字节（`02:00:00:00:19:XX`）。
两台设备**必须**不同，否则 1905 栈会把对端当自己（[AGENT_PROBE](research/AGENT_PROBE.md) 第 12 节）。

### 11.2 ARM32 模拟器（`qemu-arm-static`）**不入包**

`zte-mesh` 和 `zte-mesh-launcher` 都不装它、也不自动下载安装。包里只有原厂 ARM32 运行环境和用 `$(TARGET_CC)` 编出来的
宿主架构工具；服务能不能起来，取决于设备内核能不能直接跑 ARM32。**这是有意的取舍，不是漏装。**

| 不进包的理由 | 说明 |
|---|---|
| OpenWrt 不编它 | feed 里没有 `qemu-user`。打进包就得塞一个预编译第三方静态二进制——二进制再分发，许可与安全维护要另处理 |
| 必须与**宿主**架构匹配 | aarch64 设备要 arm64 版，x86_64 设备要 amd64 版。一份 apk 覆盖不了两种设备，而 `zte-mesh` 是 `arch: all` |
| 体积不划算 | 静态 qemu-arm 约 11 MB，比整个运行环境包还大；目标设备根本用不到 |
| 只有一处真的需要 | 只有 chroot 里的 `db-engine-probe`（原厂 ARM32）。`ns-run`/`mesh-search` 由 launcher 现编，本来就是宿主架构 |

init 怎么判定（`zte-mesh.init` 的 `emulator` 段）：

| 情况 | 判定 | 结果 |
|---|---|---|
| `aarch64` / `arm64` | 先读 `/proc/config.gz`（退化 `/proc/config`）的 `CONFIG_COMPAT=y`；**读不到就直接 exec 一次 `$ROOT/lib/ld-linux.so.3`**，能打印自己的 usage（匹配 `*sage:*`）就认定支持 | 支持 → 原生跑 |
| `x86_64` 及其他 | 一律不可能原生 | **必须**有模拟器 |
| 任意 | `zte_mesh.main.emulator=<路径>` | 跳过探测，用指定路径（相对私有根） |
| 任意 | `emulator=none` | 强制原生。只用于诊断，不是解决办法 |

**"没有答案"不等于"不支持"**：多数固件（ax3600 上的 ImmortalWrt 6.12.103 就是）不导出 `/proc/config.gz`，早期版本因此
一律走上模拟器，而包里没有模拟器，服务就每 5 秒退一次。所以探测改成问内核，而不是猜。

私有根默认 `/usr/lib/zte-mesh/root`（`ZTE_MESH_ROOT` 可改）。模拟器是在 chroot **之后**被 exec 的，必须放在私有根内部；
init 依次找 `/usr/lib/zte-mesh/root/{,usr/bin/,bin/}qemu-arm-static` 三处。找不到会打一条完整说明（含宿主架构与三条出路），
随后 `execv errno=8`（`ENOEXEC`）。**看到 `errno=8` 先按这一节处理，不要当成代码 bug。**

三条出路，按推荐顺序：

1. **刷带 `CONFIG_COMPAT=y` 的固件**——真机唯一干净的解法：arm64 内核开了 COMPAT 就能原生跑 ARM32，不需要模拟器，
   `ZTE_MESH_EMULATED_LAB` 也不会被设置（少了两条仿真专用路径）。配置在 `.github/workflows/build-firmware.yml`。
2. **手工放一个模拟器**——实验室和临时验证用，取**与设备同架构**的一份：

   ```sh
   apt-get download qemu-user-static && dpkg-deb -x qemu-user-static_*.deb /tmp/q
   scp /tmp/q/usr/bin/qemu-arm-static root@<设备>:/usr/lib/zte-mesh/root/qemu-arm-static
   ssh root@<设备> chmod 755 /usr/lib/zte-mesh/root/qemu-arm-static
   ```

   这份文件不在任何仓库里，也不随包分发。
3. **`emulator=none`**——确认"这块板子到底能不能原生跑"，不是解决办法。

放好后确认：

```sh
ls -l /usr/lib/zte-mesh/root/qemu-arm-static      # 存在且可执行
service zte-mesh restart; sleep 5
logread | grep -e qemu -e execv                   # 不该再出现 "no qemu-arm-static was found"
```

宿主机上装的 `qemu-user-static` + `binfmt-support` 只方便**在沙箱外**直接跑 ARM 程序；服务是 chroot 后 exec，即便 binfmt
带 `F` 标志能跨 chroot 生效，解释器文件仍须在根内可见。
