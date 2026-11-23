#!/usr/bin/env python3
"""frames.py：按帧长与 CMDU 类型汇总一段 1905 pcap。

为什么单独写这个：`tcpdump -e` 的文本输出在流式管道里会被 hex dump 打断，
`awk '{print $NF}'` 取"最后一个字段当长度"实际取到的是 dump 碎片——771 字节的
M1 会被读成 68 字节，于是得出"帧根本没发出去"的错误结论（2026-11-01 真踩过）。
这里直接读 pcap 记录头里的 incl_len，不经过任何文本表示。

用法：
    python tools/capture/frames.py out/full.pcap            # 长度分布 + 类型分布
    python tools/capture/frames.py out/full.pcap --min 120  # 只看大帧（M1/M2 在这档）
    python tools/capture/frames.py out/full.pcap --cmdu 9 --tlv
    python tools/capture/frames.py out/full.pcap --dir e3    # 只看某个 AL MAC 末字节发出的
    python tools/capture/frames.py out/full.pcap --vendor    # ACD Vendor 阶段（0x0100/0x0101）

退出码 1 = 文件读不动；0 = 正常（哪怕 0 帧）。
"""

import sys

ETHER_1905 = 0x893A
CMDU_NAMES = {
    0x0000: "TopologyDiscovery",
    0x0001: "TopologyNotification",
    0x0002: "TopologyQuery",
    0x0003: "TopologyResponse",
    0x0004: "VendorSpecific",
    0x0005: "LinkMetricQuery",
    0x0006: "LinkMetricResponse",
    0x0007: "APAutoconfigSearch",
    0x0008: "APAutoconfigResponse",
    0x0009: "APAutoconfigWSC",
    0x000A: "APCapabilityQuery",
    0x000B: "APCapabilityReport",
    0x8000: "APCapabilityQuery",
    0x8001: "APCapabilityReport",
    0x8006: "ChannelSelectionRequest",
    0x000E: "HigherLayerResponse",
    0x8003: "PolicyConfigRequest",
    0x8007: "ChannelSelectionResponse",
    0x800C: "APMetricsResponse",
    0x8014: "ClientSteeringRequest",
}
WSC_ATTRIBUTES = {
    0x104A: "Version", 0x1022: "MessageType", 0x1047: "UUID-E", 0x1020: "MAC",
    0x101A: "EnrolleeNonce", 0x1032: "PublicKey", 0x1004: "AuthTypeFlags",
    0x1010: "EncryptTypeFlags", 0x100D: "ConnectionType", 0x1008: "ConfigMethods",
    0x1044: "WPSState", 0x1021: "Manufacturer", 0x1023: "ModelName",
    0x1024: "ModelNumber", 0x1011: "DeviceName", 0x1042: "SerialNumber",
    0x1054: "PrimaryDeviceType", 0x103C: "RFBands", 0x1002: "AssocState",
    0x1012: "DevicePasswordID", 0x1009: "ConfigError", 0x102D: "OSVersion",
    0x1045: "SSID", 0x106A: "RequestToEnroll",
}


def read_pcap(path):
    """返回 [frame, ...]；只认 libpcap 的经典格式（这台机上 tcpdump 写的都是它）。"""
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < 24:
        raise ValueError("文件太短，不是 pcap")
    magic = blob[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        order, nano = "<", False
    elif magic == b"\xa1\xb2\xc3\xd4":
        order, nano = ">", False
    elif magic == b"\x4d\x3c\xb2\xa1":
        order, nano = "<", True
    elif magic == b"\xa1\xb2\x3c\x4d":
        order, nano = ">", True
    else:
        raise ValueError(f"不认识的 pcap magic {magic.hex()}（pcapng 要先转换）")
    import struct
    network = struct.unpack_from(order + "I", blob, 20)[0]
    if network != 1:
        raise ValueError(f"链路类型 {network} 不是以太网")
    offset, frames = 24, []
    while offset + 16 <= len(blob):
        _, _, incl, _ = struct.unpack_from(order + "4I", blob, offset)
        offset += 16
        frames.append(blob[offset:offset + incl])
        offset += incl
    return frames


def mac_text(raw):
    return ":".join(f"{byte:02x}" for byte in raw)


def cmdu_of(frame):
    if len(frame) < 18:
        return None
    return (frame[16] << 8) | frame[17]


def is_1905(frame):
    return len(frame) >= 14 and ((frame[12] << 8) | frame[13]) == ETHER_1905


def walk_tlvs(frame, start=22):
    """CMDU 头 8 字节（version/reserved/type/id/fragment/flags）之后是 TLV 链。"""
    offset = start
    while offset + 3 <= len(frame):
        kind = frame[offset]
        length = (frame[offset + 1] << 8) | frame[offset + 2]
        if offset + 3 + length > len(frame):
            return
        yield kind, frame[offset + 3:offset + 3 + length]
        offset += 3 + length
        if kind == 0:
            return


def show_tlvs(frame):
    for kind, value in walk_tlvs(frame):
        note = ""
        if kind == 0:
            note = "EndOfMessage"
        elif kind == 0x0B and len(value) >= 5 and value[:3] == b"\x00\x19\xc6":
            note = f"Vendor OUI 0019c6 sub=0x{(value[3] << 8) | value[4]:04x}"
        elif kind == 0x11:
            note = "WSC TLV"
        elif kind == 0x85:
            note = "AP Radio Identifier"
        print(f"      tlv 0x{kind:02x} len={len(value):4d}  {note}")
        if kind != 0x11:
            continue
        offset = 0
        while offset + 4 <= len(value):
            attribute = (value[offset] << 8) | value[offset + 1]
            length = (value[offset + 2] << 8) | value[offset + 3]
            body = value[offset + 4:offset + 4 + length]
            name = WSC_ATTRIBUTES.get(attribute, f"0x{attribute:04x}")
            if attribute in (0x1021, 0x1023, 0x1024, 0x1011, 0x1042, 0x1045):
                # The NUL is split out before the f-string: an expression with a backslash there is a syntax
                # error until Python 3.12, and this script also runs on the lab VM's 3.10.
                text = body.split(b"\x00")[0]
                print(f"        {name:16s} = {text!r}")
            elif attribute in (0x1032, 0x101A, 0x1047):
                print(f"        {name:16s} len={length} head={body[:8].hex()}")
            elif length <= 4:
                print(f"        {name:16s} = 0x{int.from_bytes(body, 'big'):x}")
            else:
                print(f"        {name:16s} len={length}")
            offset += 4 + length


def vendor_body(frame):
    """返回 (子类型, 载荷)；不是中兴 VendorSpecific 就返回 None。

    帧布局：14 字节以太头 + 8 字节 CMDU 头（version/reserved/type/id/frag/flags）
    + TLV。第一个 TLV 是 0x0b（Vendor Specific），value = OUI(3) + 子类型(2) + 载荷。
    子类型 0x0100 / 0x0101 分别对应 2.4G / 5G 两条 ACD 通道。
    """
    if cmdu_of(frame) != 0x0004:
        return None
    for kind, value in walk_tlvs(frame):
        if kind == 0:
            return None
        if kind == 0x0B and len(value) >= 5 and value[:3] == b"\x00\x19\xc6":
            return ((value[3] << 8) | value[4], value[5:])
    return None


def show_vendor(frames):
    """把 ACD Vendor 阶段按时间排开，并逐条判断有没有人应答。

    阶段语义（multiapd 0x71dc0/0x7c6f8 反汇编结论）：Vendor 是 ACD Client 在拿到 M2
    之后的第三段，按 opcode 0 → 1 → 2 三轮问答；每轮请求的载荷首字节就是 opcode。
    队列（ctx[band]+0x58 链表）走空时会直接转 state 4 Complete，所以「opcode 序列
    反复从 0 重来」等价于「队列每轮被重置、永远走不完」。
    """
    events = []
    for frame in frames:
        parsed = vendor_body(frame)
        if parsed is None:
            continue
        subtype, body = parsed
        if not body:
            continue
        events.append({
            "src": mac_text(frame[6:12]),
            "dst": mac_text(frame[0:6]),
            "sub": subtype,
            "op": body[0],
            "size": len(body),
            "hex": body.hex(),
        })
    if not events:
        print("  没有 VendorSpecific（OUI 0019c6）帧")
        return
    sides = {}
    for event in events:
        sides[event["src"]] = sides.get(event["src"], 0) + 1
    if len(sides) < 2:
        print(f"  只有一侧在发：{sides}")
        return
    # 请求方是 Enrollee，按协议永远先开口。不能拿"帧数少"当启发式：应答侧
    # 每条请求只回一条，反而可能更少（2026-11-01 真判反过）。
    requester = events[0]["src"]
    print(f"  请求方 {requester}（{sides[requester]} 帧），"
          f"应答方其余（{sum(sides.values()) - sides[requester]} 帧）")
    pending, rounds = {}, []
    for event in events:
        key = event["sub"]
        if event["src"] == requester:
            pending[key] = event
            continue
        request = pending.pop(key, None)
        if request is None:
            continue
        rounds.append((request, event))
    paired = {id(pair[0]) for pair in rounds}
    unanswered = [event for event in events if event["src"] == requester and id(event) not in paired]

    print(f"\n  {'轮':>3}  {'子类型':>7} {'请求 op':>8} {'请求长':>6} {'应答 op':>8} {'应答长':>6}")
    for index, (request, reply) in enumerate(rounds, 1):
        print(f"  {index:3d}  0x{request['sub']:04x}   0x{request['op']:02x} "
              f"{request['size']:6d}    0x{reply['op']:02x} {reply['size']:6d}")
    if unanswered:
        print(f"\n  ✗ 有 {len(unanswered)} 条请求没等到应答：")
        for event in unanswered[:6]:
            print(f"      sub=0x{event['sub']:04x} op=0x{event['op']:02x} "
                  f"len={event['size']}  hex={event['hex']}")

    for subtype in sorted({pair[0]["sub"] for pair in rounds}):
        sequence = [request["op"] for request, _ in rounds if request["sub"] == subtype]
        if len(sequence) < 6:
            continue
        # 找最短的重复前缀：0,1,2,0,1,2 → 3
        period = None
        for candidate in range(1, len(sequence) // 2 + 1):
            if all(sequence[i] == sequence[i % candidate] for i in range(len(sequence))):
                period = candidate
                break
        note = f"每 {period} 轮重复 {sequence[:period]}" if period else "不重复"
        print(f"\n  sub=0x{subtype:04x} 的 opcode 序列（{len(sequence)} 轮）：{sequence}")
        print(f"    → {note}"
              + ("（队列被反复重置，走不到队尾就不会转 state 4）" if period else ""))
    if rounds:
        print(f"\n  首轮请求载荷：{rounds[0][0]['hex']}")
        print(f"  首轮应答载荷：{rounds[0][1]['hex']}")


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    path = argv[0]
    minimum, want_cmdu, show_all, side, vendor_view = 0, None, False, None, False
    index = 1
    while index < len(argv):
        flag = argv[index]
        if flag == "--min":
            minimum = int(argv[index + 1], 0)
            index += 2
        elif flag == "--cmdu":
            want_cmdu = int(argv[index + 1], 0)
            index += 2
        elif flag == "--tlv":
            show_all = True
            index += 1
        elif flag == "--vendor":
            vendor_view = True
            index += 1
        elif flag == "--dir":
            side = argv[index + 1].lower().replace(":", "")
            index += 2
        else:
            print(f"不认识的参数 {flag}")
            return 2
    try:
        frames = read_pcap(path)
    except (OSError, ValueError) as error:
        print(f"读不了 {path}：{error}")
        return 1
    frames = [frame for frame in frames if is_1905(frame)]
    if side:
        frames = [frame for frame in frames
                  if len(frame) >= 12 and mac_text(frame[6:12]).endswith(side)]
    if want_cmdu is not None:
        frames = [frame for frame in frames if cmdu_of(frame) == want_cmdu]
    frames = [frame for frame in frames if len(frame) >= minimum]
    print(f"{path}: {len(frames)} 帧（1905"
          + (f", cmdu 0x{want_cmdu:04x}" if want_cmdu is not None else "")
          + (f", 源 MAC 末字节 {side}" if side else "") + f", >= {minimum} B）")
    if not frames:
        return 0
    sizes = {}
    for frame in frames:
        sizes[len(frame)] = sizes.get(len(frame), 0) + 1
    print("  长度分布：" + "  ".join(f"{size}B×{sizes[size]}" for size in sorted(sizes)))
    kinds = {}
    for frame in frames:
        kinds[cmdu_of(frame)] = kinds.get(cmdu_of(frame), 0) + 1
    print("  类型分布：" + "  ".join(
        f"{CMDU_NAMES.get(kind, hex(kind) if kind is not None else '?')}×{count}"
        for kind, count in sorted(kinds.items(), key=lambda pair: -pair[1])))
    for frame in frames:
        if not show_all:
            continue
        kind = cmdu_of(frame)
        print(f"\n  len={len(frame)} {mac_text(frame[6:12])} -> {mac_text(frame[0:6])} "
              f"cmdu=0x{kind:04x} {CMDU_NAMES.get(kind, '?')}")
        show_tlvs(frame)
    if vendor_view:
        print()
        show_vendor(frames)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
