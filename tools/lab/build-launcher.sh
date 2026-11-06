#!/bin/sh
# build-launcher.sh：给一台"包里没有它那一档构建"的目标机现编一份启动器，放进入口脚本会找的那个目录。
#
# 为什么还需要这个：zte-mesh 是 PKGARCH:=all，一个 apk 带 aarch64 / armv7 / x86_64 各一份，
# 装到哪台机器都收——"这台机器装不上启动器"这种失败不会再发生了。剩下的情况是**这台机器的 CPU 不在包里**：
# feed 构建树里只有目标机那一档的工具链，出来的 apk 就只带一档；或者这台是 mips 之类本包不带的架构。
# 那时 /usr/libexec/zte-mesh/{ns-run,mesh-search} 那份入口脚本会报出它的 `uname -m` 与包里带的几档，
# zte-mesh.init 因此不把实例交给 procd（否则每 5 秒重开一个 127）。
#
# 二进制要放在 `/usr/libexec/zte-mesh/arch/<目录名>/`，目录名与 launcher-dispatch 对 `uname -m` 的映射一致
# （aarch64 / armv7 / x86_64）。**放到外面那一层没用**：那两个名字属于包，`apk add` 会覆盖它们，
# 而入口脚本也只会去 arch/ 下面找。
#
# 这两个程序不依赖任何 OpenWrt 私有库，在编译机上用原生 gcc 编出来就能跑——不用交叉，
# 因为目标机和编译机同为 x86_64。跨架构请用 tools/build/launcher-arches.sh（它认交叉编译器）。
#
# 用法（Git Bash 下先 export MSYS_NO_PATHCONV=1，否则 /tmp 会被改写成 Windows 路径）：
#     tools/lab/build-launcher.sh
#     BUILD_HOST=ubuntu TARGET_HOST=owrt tools/lab/build-launcher.sh
#     SRC='~/zte-mesh' tools/lab/build-launcher.sh      # 编译机上的仓库路径
#
# 退出码：0 = 装好并自检通过；1 = 编译或传输失败；2 = 用法错误。

set -eu

BUILD_HOST=${BUILD_HOST:-ubuntu}
TARGET_HOST=${TARGET_HOST:-owrt}
PYTHON=${PYTHON:-python3}
# 引号别去掉：不加引号的话本机 shell 会把 ~ 展开成自己的家目录，再送过去就是错路径。
SRC=${SRC-'~/build'}
RSSH=${RSSH:-$(dirname "$0")/rssh.py}
# 远端和本机用的是两个不同的路径：本机 Python 是原生 Windows 解释器，它眼里的
# /tmp 是 C://tmp，跟 Git Bash 的 MSYS /tmp 不是同一个地方。所以本机侧一律用
# 项目内的相对路径，远端侧才用 /tmp。
REMOTE_WORK=${REMOTE_WORK:-/tmp/zte-lab-launcher}
LOCAL_WORK=${LOCAL_WORK:-out/lab-launcher}
LIBEXEC=${LIBEXEC:-/usr/libexec/zte-mesh}

say() { printf '%s\n' "$*"; }

if [ ! -f "$RSSH" ]; then
    say "找不到 $RSSH（用 RSSH=... 指定）" >&2
    exit 2
fi
if ! "$PYTHON" -c "import paramiko" 2>/dev/null; then
    say "$PYTHON 里没有 paramiko：pip install paramiko，或用 PYTHON=... 指定一个装了的解释器" >&2
    exit 2
fi

remote() { "$PYTHON" "$RSSH" "$1" "$2"; }

# 目录名必须与 openwrt/zte-mesh/files/launcher-dispatch 里那张 `uname -m` 映射一致，
# 否则装进去了入口脚本也找不到它。arm64 与 armv7l 都归到包用的那两个名字上。
target_machine=$(remote "$TARGET_HOST" 'uname -m')
case $target_machine in
    aarch64 | arm64) arch_dir=aarch64 ;;
    armv7*) arch_dir=armv7 ;;
    *) arch_dir=$target_machine ;;
esac
say "目标机 $TARGET_HOST 的 CPU 是 $target_machine，二进制放进 $LIBEXEC/arch/$arch_dir/"

say "== 1/4 在 $BUILD_HOST 上编译（原生 gcc；目标机 $target_machine，不同架构请改用 tools/build/launcher-arches.sh）"
# 必须 -static。两台同为 x86_64 也一样：编译机是 glibc、目标机是 musl，动态链接编出来的
# 程序在目标机上 execve 直接 "not found"（缺 /lib64/ld-linux-x86-64.so.2，不是缺文件）。
# 2026-11-01 真踩过——装上去服务起不来，日志里连一行自己的输出都没有。
remote "$BUILD_HOST" "
set -e
cd $SRC
mkdir -p $REMOTE_WORK
gcc -std=c11 -O2 -Wall -static -o $REMOTE_WORK/ns-run src/ns-run.c src/controller-net.c
gcc -std=c11 -O2 -Wall -static -Wno-unused-function -o $REMOTE_WORK/mesh-search tools/lab/enroller.c
ls -l $REMOTE_WORK/ns-run $REMOTE_WORK/mesh-search
"

say "== 2/4 取回本机（rssh 不做机对机）"
mkdir -p "$LOCAL_WORK"
for name in ns-run mesh-search; do
    "$PYTHON" "$RSSH" "$BUILD_HOST" get "$REMOTE_WORK/$name" "$LOCAL_WORK/$name"
done

say "== 3/4 装到 $TARGET_HOST 的 arch/$arch_dir/（先停服务再备份）"
# 必须先停：ns-run 是正在跑的进程，覆盖它会得到 "Text file busy"。
# 只动 arch/<cpu>/ 这一层：$LIBEXEC 上层的 ns-run / mesh-search 是包里那份入口脚本，apk add 会重写它们，
# 而入口脚本也只会去 arch/ 下面找二进制——放错地方等于没放。
remote "$TARGET_HOST" "/etc/init.d/zte-mesh stop >/dev/null 2>&1 || true
    sleep 2
    mkdir -p /tmp/zte-launcher-bak $LIBEXEC/arch/$arch_dir
    for f in ns-run mesh-search; do
        [ -f $LIBEXEC/arch/$arch_dir/\$f ] && cp -a $LIBEXEC/arch/$arch_dir/\$f /tmp/zte-launcher-bak/ || true
    done
    ls -l /tmp/zte-launcher-bak/ 2>/dev/null || true"
for name in ns-run mesh-search; do
    "$PYTHON" "$RSSH" "$TARGET_HOST" put "$LOCAL_WORK/$name" "$LIBEXEC/arch/$arch_dir/$name"
done
remote "$TARGET_HOST" "chmod 755 $LIBEXEC/arch/$arch_dir/ns-run $LIBEXEC/arch/$arch_dir/mesh-search"

say "== 4/4 自检"
# 两步各判一件事。第一步判"入口脚本会不会走到这个目录"：--launcher-check 是 zte-mesh.init 起实例前问的那一句，
# 它答的文件路径必须就是刚放进去的那一个——目录名映射错了、或者上层那个名字被换成了别的脚本，都会在这里露出来。
if ! remote "$TARGET_HOST" "$LIBEXEC/ns-run --launcher-check 2>&1 | grep -qx $LIBEXEC/arch/$arch_dir/ns-run"; then
    say "入口脚本没有选中这个文件。上层那个 ns-run 可能不是包里那份 dispatcher（手工放过的旧二进制就是这样），" >&2
    say "也可能是目录名和它对 \`uname -m\` 的映射不一致：这台报的是 $target_machine，脚本建的是 $arch_dir。" >&2
    exit 1
fi
# 第二步判能不能 exec：glibc 动态链接的程序在 musl 上会得到"not found"，而这时候服务已经
# 停了，只看 status 只会看到一句 running 然后什么都没有。这一步放在重启之前。
# 注意不能只看退出码：ns-run 对 --help 返回 64（用法错误），跑不起来时是 127。
# 认 usage 文本才可靠。
if ! remote "$TARGET_HOST" "$LIBEXEC/ns-run --help 2>&1 | grep -q '^usage: ns-run'"; then
    say "ns-run 跑不起来（多半是没加 -static，glibc 程序在 musl 上 execve 返回 ENOENT）。" >&2
    say "已装的文件留在 $LIBEXEC/arch/$arch_dir/，备份在 /tmp/zte-launcher-bak/。" >&2
    exit 1
fi
remote "$TARGET_HOST" "
echo '-- 入口归属（这两个名字属于包，apk add 会重写）：'; for f in ns-run mesh-search; do printf '   %-12s ' \"\$f\"; \
    apk info -W $LIBEXEC/\$f 2>&1 | head -1; done
echo '-- 这一档的二进制（不属于任何包）：'; ls -l $LIBEXEC/arch/$arch_dir
echo '-- 服务：'; /etc/init.d/zte-mesh restart >/dev/null 2>&1; sleep 10; \
    /etc/init.d/zte-mesh status 2>&1 | head -2
echo '-- 节点：'; head -c 200 /run/zte-mesh/state.json 2>/dev/null; echo
"

say ""
say "完成。$LIBEXEC/arch/$arch_dir/ 里这两个文件不在任何包里，apk upgrade 不会删它们，但也不会更新它们——"
say "仓库里改了 src/ns-run.c 之后要重跑本脚本；下一次 apk add 只重写上层那两份入口脚本，不动这个目录。"
