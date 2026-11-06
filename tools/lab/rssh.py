#!/usr/bin/env python3
"""三台测试机的统一 SSH 通道（密码登录）。

`tools/lab/rsh.py` 走 telnet，只服务中兴原厂路由器（ZXHN Z506 只开 telnet + 明文
密码）。本文件是另一回事：2026-11-01 起测试环境换成 ImmortalWrt 虚拟机 +
Ubuntu 虚拟机 + 小米 AX3600（刷 ImmortalWrt）三台，全部是 OpenSSH + 密码。
本机（Windows）没有 sshpass、没有 rsync，`ssh` 又不能喂密码，所以用 paramiko
当一个可脚本化的 scp/ssh。

命令行只写别名。账号与口令不在这棵树里：读本机的 `.local/lab-env.md`（已
gitignore）——那份表格每行的第 1/3/4 格是别名、用户、口令。也可以用环境变量
`ZTE_LAB_USER_<别名>` / `ZTE_LAB_PASS_<别名>` 覆盖某一台，或 `ZTE_LAB_PASS`
覆盖全部；`ZTE_LAB_ENV` 指到别的文件。

用法：
    python tools/lab/rssh.py owrt uname -a
    python tools/lab/rssh.py owrt 'uci show wireless | head'   # 管道要当一整个参数
    python tools/lab/rssh.py ax3600 put zte-mesh.apk /tmp/
    python tools/lab/rssh.py ubuntu get /var/log/x.log ./x.log
    python tools/lab/rssh.py --all uname -a

退出码：远端命令的退出码原样返回；连接失败返回 255；用法错误返回 2。
"""

from __future__ import annotations

import argparse
import os
import posixpath
import sys
import time

try:
    import paramiko
except ImportError:  # 只有 3.13 那个解释器没有，miniconda 的有
    sys.stderr.write(
        "需要 paramiko：用装了它的那个解释器跑（本机是 miniconda 的 python），或 pip install paramiko。\n"
    )
    raise SystemExit(2)

# 别名 -> (地址, 说明)。账号与口令见文件头：只从本机 .local/lab-env.md 或环境变量取。
HOSTS = {
    "ax3600": ("192.168.1.1", "小米 AX3600 / ImmortalWrt / ipq807x aarch64"),
    "owrt": ("192.168.1.20", "ImmortalWrt 虚拟机（测试环境里的主控）"),
    "ubuntu": ("192.168.1.191", "Ubuntu 虚拟机（编译机）"),
}

CONNECT_TIMEOUT = 15
CMD_TIMEOUT = 300


def _env_file() -> str:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    return os.environ.get("ZTE_LAB_ENV") or os.path.join(root, ".local", "lab-env.md")


def _local_accounts() -> dict:
    """读本机那份未入库的表格，返回 别名 -> (用户, 口令)。文件不在就返回空。"""
    found: dict = {}
    try:
        lines = open(_env_file(), encoding="utf-8").read().splitlines()
    except OSError:
        return found
    for line in lines:
        cells = [c.strip().strip("`") for c in line.strip().strip("|").split("|")]
        if len(cells) >= 4 and cells[0] in HOSTS:
            found[cells[0]] = (cells[2], cells[3])
    return found


def credentials(alias: str) -> tuple:
    user, secret = _local_accounts().get(alias, (None, None))
    upper = alias.upper()
    user = os.environ.get(f"ZTE_LAB_USER_{upper}", user)
    secret = os.environ.get(f"ZTE_LAB_PASS_{upper}") or os.environ.get("ZTE_LAB_PASS") or secret
    if not user or not secret:
        raise SystemExit(
            f"{alias}: 没有账号或口令。在 {_env_file()} 的表格里补一行"
            f"「| `{alias}` | 地址 | 用户 | 口令 | … |」，或设环境变量"
            f" ZTE_LAB_USER_{upper} / ZTE_LAB_PASS_{upper}。"
        )
    return user, secret


def connect(alias: str) -> paramiko.SSHClient:
    host = HOSTS[alias][0]
    user, secret = credentials(alias)
    cli = paramiko.SSHClient()
    # 测试机随时重刷，host key 每次都会变；这里信任并记录，不做 pin。
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cli.connect(
        host,
        port=22,
        username=user,
        password=secret,
        timeout=CONNECT_TIMEOUT,
        banner_timeout=CONNECT_TIMEOUT,
        auth_timeout=CONNECT_TIMEOUT,
        look_for_keys=False,
        allow_agent=False,
    )
    return cli


def run(alias: str, command: str, quiet: bool = False, pty: bool = False) -> int:
    cli = connect(alias)
    try:
        # 默认**不开** pty：两台 OpenWrt 跑的是 dropbear，带 pty 的 exec 会把
        # 命令当成登录 shell 的输入，`echo HI` 都返回 127。需要 tty 的少数命令
        # （opkg 交互、top）用 --pty 显式要。
        stdin, stdout, stderr = cli.exec_command(command, timeout=CMD_TIMEOUT, get_pty=pty)
        stdin.close()
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
        if not quiet:
            sys.stdout.write(out)
            if err.strip():
                sys.stdout.write(err)
            sys.stdout.flush()
        return code
    finally:
        cli.close()


def _sftp(alias: str):
    """开 sftp；拿不到就返回 None，交给 exec 通道。

    AX3600 的固件是 dropbear + 极简 image，**没有 sftp-server**（openssh-sftp-server
    未装），`open_sftp()` 会在协商阶段抛 "EOF during negotiation"。这不是连接问题，
    所以不能当失败退出 —— 退回 `cat` 管道即可，对二进制一样安全（SSH 通道是 8-bit
    clean 的，不做换行转换）。
    """
    cli = connect(alias)
    try:
        return cli, cli.open_sftp()
    except Exception:
        return cli, None


def _exec_put(cli, local: str, remote: str) -> int:
    """Write a local file over the exec channel. Used when sftp negotiates but cannot open a path."""
    data = open(local, "rb").read()
    stdin, stdout, stderr = cli.exec_command(f"cat > '{remote}'", timeout=CMD_TIMEOUT)
    stdin.write(data)
    stdin.channel.shutdown_write()
    err = stderr.read().decode("utf-8", "replace")
    if stdout.channel.recv_exit_status() != 0:
        sys.stderr.write(f"写入失败：{err}\n")
        return 1
    _, out, _ = cli.exec_command(f"wc -c < '{remote}'", timeout=30)
    return int(out.read().decode().strip() or -1)


def put(alias: str, local: str, remote: str) -> int:
    if not os.path.exists(local):
        sys.stderr.write(f"本地不存在：{local}\n")
        return 2
    cli, sftp = _sftp(alias)
    try:
        if remote.endswith("/"):
            # SFTP 的 put 不会替你拼文件名，目录要自己接。
            remote = posixpath.join(remote, os.path.basename(local))
        size = -1
        if sftp is not None:
            try:
                sftp.put(local, remote)
                size = sftp.stat(remote).st_size
            except Exception as exc:
                # The Ubuntu VM answers the sftp handshake but refuses every path with ENOENT, so a working
                # negotiation is not proof that sftp works. Fall back to the exec channel rather than make the
                # caller guess which of the three machines needs that.
                sys.stderr.write(f"{alias}: sftp 不可用（{exc}），改走 cat 通道\n")
        if size < 0:
            size = _exec_put(cli, local, remote)
        local_size = os.path.getsize(local)
        print(f"{alias}:{remote}  {size} B" + ("" if size == local_size else f"  != {local_size} B"))
        return 0 if size == local_size else 1
    finally:
        cli.close()


def get(alias: str, remote: str, local: str) -> int:
    if local.endswith(("/", os.sep)):
        os.makedirs(local, exist_ok=True)
        local = os.path.join(local, posixpath.basename(remote))
    cli, sftp = _sftp(alias)
    try:
        got = False
        if sftp is not None:
            try:
                sftp.get(remote, local)
                got = True
            except Exception as exc:
                sys.stderr.write(f"{alias}: sftp 不可用（{exc}），改走 cat 通道\n")
        if not got:
            _, out, err = cli.exec_command(f"cat '{remote}'", timeout=CMD_TIMEOUT)
            data = out.read()
            if out.channel.recv_exit_status() != 0:
                sys.stderr.write(err.read().decode("utf-8", "replace"))
                return 1
            open(local, "wb").write(data)
        print(f"{local}  {os.path.getsize(local)} B")
        return 0
    finally:
        cli.close()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        add_help=True,
        description="三台测试机的 SSH/scp（密码登录）",
        epilog="命令部分不归 argparse 管：第一个非选项 token 起，后面整串原样交给远端 shell。",
    )
    ap.add_argument("host", nargs="?", choices=sorted(HOSTS) + ["list"], help="别名，或 list")
    ap.add_argument("--all", action="store_true", help="对三台依次执行")
    ap.add_argument("--quiet", action="store_true", help="不打印远端输出，只看退出码")
    ap.add_argument("--bench", action="store_true", help="打印每台的 build 信息（架构/内核/发行版）")
    ap.add_argument("--pty", action="store_true", help="要一个 tty（少数交互命令）")

    # 为什么不用 argparse 的 REMAINDER：它会把 `--sh`/`--all` 这些控制参数也一起
    # 吞进 rest（实测 Namespace(rest=['--sh', 'echo HI'])），于是远端收到的是
    # `--sh echo HI`，ash 直接回 "ash: -c requires an argument"，退出码 2 —— 一
    # 个看起来像"连不上"的假故障。所以只把手上的已知选项吃掉，其余原样保留。
    flags = {"--all", "--quiet", "--bench", "--pty"}
    opts: list[str] = []
    command: list[str] = []
    it = iter(argv)
    for tok in it:
        if tok == "--":
            command.extend(it)
            break
        if tok in flags:
            opts.append(tok)
        elif tok.startswith("--") and not command:
            sys.stderr.write(f"未知选项：{tok}\n")
            return 2
        else:
            command.append(tok)
    a = ap.parse_args(opts + ([command[0]] if command and command[0] in list(HOSTS) + ["list"] else []))
    if command and command[0] in list(HOSTS) + ["list"]:
        command = command[1:]

    if a.host == "list" or (a.host is None and not a.all and not a.bench):
        accounts = _local_accounts()
        for k, (h, d) in HOSTS.items():
            have = "凭据有" if (k in accounts or os.environ.get("ZTE_LAB_PASS")) else "凭据缺"
            print(f"{k:8s} {h:15s} {have}  {d}")
        return 0

    targets = sorted(HOSTS) if (a.all or a.host is None) else [a.host]
    if a.bench:
        cmd = (
            'echo "## $(uname -n)"; uname -m; '
            'grep -m1 "model name\\|Hardware\\|system type" /proc/cpuinfo 2>/dev/null; '
            'uname -r; cat /etc/openwrt_release 2>/dev/null | head -3; '
            'cat /etc/os-release 2>/dev/null | head -2; '
            'nproc; free -m 2>/dev/null | head -2'
        )
        for t in targets:
            print(f"=== {t} ({HOSTS[t][0]})")
            run(t, cmd)
            print()
        return 0

    rest = command
    if not rest:
        sys.stderr.write("没有命令。例：rssh.py owrt 'uname -a'\n")
        return 2

    verb = rest[0]
    if verb in ("put", "get"):
        if len(rest) < 3:
            sys.stderr.write(f"用法：{verb} <src> <dst>\n")
            return 2
        rc = 0
        for t in targets:
            rc |= (put if verb == "put" else get)(t, rest[1], rest[2])
        return rc

    cmd = " ".join(rest)
    rc = 0
    for t in targets:
        if a.all:
            print(f"=== {t}")
        rc |= run(t, cmd, quiet=a.quiet, pty=a.pty)
    return rc


if __name__ == "__main__":
    t0 = time.time()
    sys.exit(main(sys.argv[1:]))
