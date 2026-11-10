#!/usr/bin/env python3
"""test_frames.py：tools/capture/frames.py 的回归测试。

重点钉住 2026-11-01 踩过的两件事：

1. **帧长必须从 pcap 记录头读**。`tcpdump -e` 的文本输出会被 hex dump 打断，
   `awk '{print $NF}'` 拿到的是 dump 碎片，把 771 字节的 M1 读成 68 字节，
   于是得出「M1 没上以太网」的错结论。
2. **Vendor 阶段的重复周期要能自动看出来**。ACD Client 拿到 M2 之后走三段
   opcode 0/1/2 问答，队列（ctx[band]+0x58 链表）走到队尾才转 state 4；
   实测序列是 0,1,2,0,1,2…，意味着队列每轮被重置，永远不 Complete。

用合成 pcap，不依赖真机抓包。帧布局按实测反推：
    14 以太头 + 8 CMDU 头 + [3 TLV 头 + 5 OUI/子类型 + body] + 3 EOM
    → 总长 = 30 + len(body)，与真机 34/42/52/60/68 逐一对得上。

退出码 1 = 失败；0 = 通过。
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(HERE, "..", "tools", "capture", "frames.py")

ENROLLEE = bytes.fromhex("0200000019e3")
REGISTRAR = bytes.fromhex("020000001903")
BROADCAST = b"\xff" * 6


def ethernet(dst, src, payload, ethertype=0x893A):
    return dst + src + struct.pack(">H", ethertype) + payload


def cmdu(message_type, message_id, tlvs):
    """CMDU 头 8 字节：version(1) reserved(1) type(2) id(2) fragment(1) flags(1)。"""
    body = b""
    for kind, value in tlvs:
        body += bytes([kind]) + struct.pack(">H", len(value)) + value
    body += b"\x00\x00\x00"                      # EndOfMessage
    return bytes([0x00, 0x00]) + struct.pack(">HH", message_type, message_id) + b"\x00\x00" + body


def vendor_frame(src, dst, subtype, payload, message_id=1):
    value = bytes.fromhex("0019c6") + struct.pack(">H", subtype) + payload
    return ethernet(dst, src, cmdu(0x0004, message_id, [(0x0B, value)]))


def write_pcap(path, frames):
    with open(path, "wb") as handle:
        handle.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for index, frame in enumerate(frames):
            handle.write(struct.pack("<IIII", 1700000000 + index, 0, len(frame), len(frame)))
            handle.write(frame)


def run(path, *flags):
    # The report carries 240B×1 and Chinese; on a GBK console the child would write those bytes in GBK and the
    # parent's UTF-8 decode would hand back None stdout -- pin both ends to UTF-8 so the assertion sees text.
    result = subprocess.run([sys.executable, FRAMES, path] + list(flags),
                            capture_output=True, text=True, encoding="utf-8",
                            env={**os.environ, "PYTHONUTF8": "1"})
    if result.returncode != 0:
        raise AssertionError(f"frames.py 退出码 {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def test_frame_layout():
    """合成的 1 字节 Vendor 帧必须正好 34 字节，和真机抓包同长。"""
    frame = vendor_frame(ENROLLEE, REGISTRAR, 0x0100, b"\x00")
    assert len(frame) == 34, f"帧长 {len(frame)}，真机同款是 34"
    frame = vendor_frame(ENROLLEE, REGISTRAR, 0x0100, b"\x01" + b"\x00" * 18)
    assert len(frame) == 52, f"19 字节载荷应为 52，实得 {len(frame)}"


def test_lengths_come_from_record_header():
    """长度必须来自 pcap 记录头，不能受帧内容影响。"""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "a.pcap")
        write_pcap(path, [
            ethereum_with_length(240),
            ethereum_with_length(771),
            ethereum_with_length(911),
        ])
        out = run(path)
        # tcpdump 文本法会把这几条读成同一个数；这里必须三个都在
        assert "240B×1" in out, out
        assert "771B×1" in out, out
        assert "911B×1" in out, out


def ethereum_with_length(size):
    """造一条指定总长的 1905 帧：14 以太 + 8 CMDU 头 + 3 TLV 头 + 载荷 + 3 EOM。"""
    filler = size - 14 - 8 - 3 - 3
    assert filler >= 0, f"{size} 太短，装不下 1905 头"
    tlv = bytes([0x11]) + struct.pack(">H", filler) + b"\x00" * filler
    payload = (bytes([0x00, 0x00]) + struct.pack(">HH", 0x0009, 1)
               + b"\x00\x00" + tlv + b"\x00\x00\x00")
    assert 14 + len(payload) == size, f"造出来 {14 + len(payload)} 字节，要 {size}"
    return ethernet(BROADCAST, ENROLLEE, payload)


def test_vendor_rounds_pair_up():
    """三轮 op 0/1/2 必须两两配对，且被识别成「每 3 轮重复」。"""
    request_sizes = {0x00: 1, 0x01: 9, 0x02: 19}
    reply_sizes = {0x00: 19, 0x01: 35, 0x02: 1}
    frames, message_id = [], 1
    for _ in range(3):                                  # 3 轮
        for subtype in (0x0100, 0x0101):
            for op in (0x00, 0x01, 0x02):
                payload = bytes([op]) + b"\x00" * (request_sizes[op] - 1)
                frames.append(vendor_frame(ENROLLEE, REGISTRAR, subtype, payload, message_id))
                message_id += 1
                reply = bytes([op]) + b"\x00" * (reply_sizes[op] - 1)
                frames.append(vendor_frame(REGISTRAR, ENROLLEE, subtype, reply, message_id))
                message_id += 1
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "v.pcap")
        write_pcap(path, frames)
        out = run(path, "--vendor")
    assert "请求方 02:00:00:00:19:e3" in out, out
    assert "每 3 轮重复 [0, 1, 2]" in out, out
    assert "没等到应答" not in out, out


def test_vendor_reports_missing_reply():
    """少了应答必须报出来，而不是静默配对到下一条。"""
    frames = [
        vendor_frame(ENROLLEE, REGISTRAR, 0x0100, b"\x00", 1),
        vendor_frame(REGISTRAR, ENROLLEE, 0x0100, b"\x00" + b"\x00" * 18, 2),
        vendor_frame(ENROLLEE, REGISTRAR, 0x0100, b"\x01" + b"\x00" * 8, 3),   # 故意没有应答
    ]
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "lost.pcap")
        write_pcap(path, frames)
        out = run(path, "--vendor")
    assert "没等到应答" in out, out


def main():
    failures = 0
    for name, case in sorted(globals().items()):
        if not name.startswith("test_") or not callable(case):
            continue
        try:
            case()
            print(f"ok    {name}")
        except AssertionError as error:
            failures += 1
            print(f"FAIL  {name}\n      {error}")
    print(f"\n{len([n for n in globals() if n.startswith('test_')]) - failures} 通过，{failures} 失败")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
