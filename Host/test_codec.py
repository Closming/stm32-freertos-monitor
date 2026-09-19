"""协议层离线自测 —— 不需要硬件，直接 ``python test_codec.py``。

这是本项目在硬件到货前**唯一能真正验证的部分**。测试通过 = 协议实现正确，
测试不通过 = 下位机的 C 实现大概率也有同样的毛病（两边是照同一份规范写的）。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from frame_codec import (
    CRC16_TABLE,
    DATA_LEN,
    FRAME_LEN,
    HEADER,
    FrameParser,
    build_frame,
    crc16_modbus,
)
from sim_frames import (
    bad_length,
    chunked,
    corrupt_crc,
    good_frame,
    good_stream,
    garbage,
    messy_stream,
    truncated,
)

# ---------------------------------------------------------------- 测试框架

_results: list[tuple[str, bool, str]] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    _results.append((name, bool(condition), detail))


def summarize() -> int:
    passed = sum(1 for _, ok, _ in _results if ok)
    failed = [(n, d) for n, ok, d in _results if not ok]

    print("=" * 68)
    for name, ok, detail in _results:
        mark = "PASS" if ok else "FAIL"
        line = f"[{mark}] {name}"
        # 只在失败时打印 detail。check() 的 detail 语义是"期望 vs 实际"，
        # 通过时打出来会出现 "PASS ... 没找到 xxx" 这种自相矛盾的行。
        if detail and not ok:
            line += f"   —— {detail}"
        print(line)
    print("=" * 68)
    print(f"通过 {passed}/{len(_results)}")

    if failed:
        print("\n失败项:")
        for name, detail in failed:
            print(f"  · {name}  {detail}")
        return 1
    return 0


def parse_all(stream: bytes) -> FrameParser:
    p = FrameParser()
    p.feed(stream)
    return p


# ---------------------------------------------------------------- 1. CRC

def test_crc():
    # CRC-16/MODBUS 的标准校验值：对 ASCII "123456789" 应得 0x4B37
    got = crc16_modbus(b"123456789")
    check("CRC16-Modbus 标准校验向量", got == 0x4B37,
          f"期望 0x4B37，得到 0x{got:04X}")

    # 空输入应返回初值
    check("CRC16 对空输入返回初值 0xFFFF", crc16_modbus(b"") == 0xFFFF)

    # 单字节翻转必须改变 CRC（否则校验形同虚设）
    a, b = crc16_modbus(b"\x01\x02\x03"), crc16_modbus(b"\x01\x02\x04")
    check("CRC16 对单比特变化敏感", a != b)


# ---------------------------------------------------------------- 2. 打包

def test_build():
    f = good_frame(0)
    check("单帧长度 = 14", len(f) == FRAME_LEN, f"实际 {len(f)}")
    check("帧头正确", f[:2] == HEADER, f"实际 {f[:2].hex(' ')}")
    check("LEN 字段 = 8", f[2] == DATA_LEN, f"实际 {f[2]}")

    probe = build_frame(0x5A, -1234, 6789, 4000, 1)
    check("打包后长度仍为 14", len(probe) == FRAME_LEN)
    check("SEQ 字段正确", probe[3] == 0x5A, f"实际 0x{probe[3]:02X}")

    # 负数温度必须能正确编码（最高位为 1，跨大端两个字节）
    assert probe[4] & 0x80, "负数温度的高字节符号位应为 1"
    check("负数温度符号位正确", True)


# ---------------------------------------------------------------- 3. 往返

def test_roundtrip():
    cases = [
        (0, 2550, 6000, 2048, 1024),
        (255, -4000, 0, 0, 0),        # 量程下限
        (1, 12500, 10000, 4095, 4095),  # 量程上限
        (128, -1234, 5678, 1, 4094),    # 随机中间值
    ]
    bad: list[str] = []
    for seq, t, h, light, pot in cases:
        p = FrameParser()
        frames = p.feed(build_frame(seq, t, h, light, pot))
        if len(frames) != 1:
            bad.append(f"seq={seq} 解析出 {len(frames)} 帧")
            continue
        f = frames[0]
        got = (f.seq, round(f.temp_c * 100), round(f.humidity * 100), f.light, f.pot)
        if got != (seq, t, h, light, pot):
            bad.append(f"seq={seq} 期望 {(seq, t, h, light, pot)} 得到 {got}")

    check("四组边界值打包→解析往返一致", not bad, "; ".join(bad))


# ---------------------------------------------------------------- 4. 流式

def test_streaming():
    # 半截帧不应产生输出，补齐后才产生
    whole = good_frame(9)
    p = FrameParser()
    early = p.feed(truncated(whole, 10))
    check("半截帧不产生输出", early == [] and p.frames_ok == 0,
          f"得到了 {len(early)} 帧")

    late = p.feed(whole[10:])
    check("补齐后半帧被正确解析", len(late) == 1 and p.frames_ok == 1,
          f"得到 {len(late)} 帧")

    # 一次喂三帧半 —— 应得到 3 帧，剩余半帧留在状态机里
    three_and_half = good_frame(1) + good_frame(2) + good_frame(3) + good_frame(4)[:7]
    p = FrameParser()
    got = p.feed(three_and_half)
    check("一次喂三帧半，解析出 3 帧", len(got) == 3, f"得到 {len(got)} 帧")
    check("剩余半帧不丢失（补上后成帧）",
          len(p.feed(good_frame(4)[7:])) == 1)


# ---------------------------------------------------------------- 5. 坏帧

def test_bad_frames():
    p = parse_all(corrupt_crc(good_frame(0)))
    check("CRC 错帧被拒绝", p.frames_ok == 0 and p.crc_errors == 1,
          f"ok={p.frames_ok} crc_err={p.crc_errors}")

    p = parse_all(bad_length(0))
    check("非法 LEN 被拒绝", p.frames_ok == 0 and p.len_errors == 1,
          f"ok={p.frames_ok} len_err={p.len_errors}")

    # 全是坏帧时绝不能"蒙对"任何一帧
    stream = b"".join(corrupt_crc(good_frame(i)) for i in range(20))
    p = parse_all(stream)
    check("连喂 20 个 CRC 错帧，接受数为 0",
          p.frames_ok == 0 and p.crc_errors == 20,
          f"ok={p.frames_ok} crc_err={p.crc_errors}")


# ---------------------------------------------------------------- 6. 重同步

def test_resync():
    # 帧头前夹垃圾
    stream = garbage(9, seed=1) + good_frame(0)
    p = parse_all(stream)
    check("帧头前夹垃圾仍能解析出帧", p.frames_ok == 1,
          f"ok={p.frames_ok}")
    check("垃圾字节被计数", p.discarded > 0, f"discarded={p.discarded}")

    # 垃圾里混 0xAA（假帧头）——最容易骗过解析器的情形
    stream = garbage(12, seed=2, avoid_header=False) + good_frame(1)
    p = parse_all(stream)
    check("垃圾中含假帧头仍能恢复同步", p.frames_ok == 1,
          f"ok={p.frames_ok}")

    # 连续 0xAA 后接真帧头：0xAA 0xAA 0x55 ...
    stream = bytes([0xAA, 0xAA]) + good_frame(2)
    p = parse_all(stream)
    check("连续 0xAA 后跟真帧头可恢复", p.frames_ok == 1, f"ok={p.frames_ok}")


# ---------------------------------------------------------------- 7. 丢包统计

def test_packet_loss():
    p = parse_all(b"".join(good_stream(10, start_seq=0, with_gaps=True, seed=7)))
    expected_dropped = p.dropped
    check("丢帧被统计到", expected_dropped > 0, f"dropped={expected_dropped}")

    # 无跳号时丢包率必须为 0
    p = parse_all(b"".join(good_stream(10, start_seq=0, with_gaps=False)))
    check("无跳号时丢包率为 0", p.dropped == 0 and p.packet_loss == 0.0,
          f"dropped={p.dropped}")
    check("无跳号时 10 帧全收", p.frames_ok == 10, f"ok={p.frames_ok}")

    # seq 从 255 回绕到 0 不应被算成丢帧
    wrap = good_frame(254) + good_frame(255) + good_frame(0) + good_frame(1)
    p = parse_all(wrap)
    check("SEQ 从 255 回绕到 0 不误判丢帧",
          p.dropped == 0 and p.frames_ok == 4,
          f"dropped={p.dropped} ok={p.frames_ok}")


# ---------------------------------------------------------------- 8. 确定性混合流

def test_deterministic_mix():
    """完全确定的混合流，逐项断言精确结果 —— 不依赖随机种子。

    这一组是协议层的"验收测试"。随机压力测试（下面那组）只能验不变量，
    验不了具体数值；真要证明"CRC 错帧不被接受、LEN 错帧不被接受、
    拒掉的帧要被算进丢包"，必须用确定的输入。
    """
    stream = (
        good_frame(0)                                       # 好
        + garbage(5, seed=1) + good_frame(1)                # 垃圾 + 好
        + corrupt_crc(good_frame(2))                        # CRC 错 → 拒
        + good_frame(3)                                     # 好
        + bad_length(4)                                     # LEN 错 → 拒
        + good_frame(5)                                     # 好
    )

    p = FrameParser()
    frames = p.feed(stream)

    check("确定性流：接受 4 帧", len(frames) == 4 and p.frames_ok == 4,
          f"得到 {len(frames)} 帧")
    check("确定性流：接受的是 seq 0,1,3,5",
          [f.seq for f in frames] == [0, 1, 3, 5],
          f"实际 {[f.seq for f in frames]}")
    check("确定性流：CRC 错计数 = 1", p.crc_errors == 1, f"实际 {p.crc_errors}")
    check("确定性流：LEN 错计数 = 1", p.len_errors == 1, f"实际 {p.len_errors}")
    # 2 和 4 被拒 → 接收端看到序号从 1 跳到 3、再从 3 跳到 5，共丢 2 帧
    check("确定性流：丢帧计数 = 2", p.dropped == 2, f"实际 {p.dropped}")
    check("确定性流：丢包率 = 2/6",
          abs(p.packet_loss - 2 / 6) < 1e-9, f"实际 {p.packet_loss:.4f}")

    # 同一段流切碎喂入，结果必须分毫不差
    p2 = FrameParser()
    got2 = []
    for chunk in chunked(stream, size=3, seed=5):
        got2 += p2.feed(chunk)
    check("确定性流：切碎喂入结果一致",
          [f.seq for f in got2] == [0, 1, 3, 5] and p2.dropped == 2,
          f"seq={[f.seq for f in got2]} dropped={p2.dropped}")


# ---------------------------------------------------------------- 9. 混杂流 + 分块

def test_messy_and_chunking():
    stream = messy_stream(40, seed=42)

    whole = parse_all(stream)

    check("混杂流中解析出有效帧", whole.frames_ok > 0, f"ok={whole.frames_ok}")
    check("混杂流触发过 CRC 错误路径", whole.crc_errors > 0,
          f"crc_err={whole.crc_errors}")
    check("混杂流触发过非法长度路径", whole.len_errors > 0,
          f"len_err={whole.len_errors}")
    check("混杂流触发过丢帧统计", whole.dropped > 0, f"dropped={whole.dropped}")

    # ★ 最关键的一条：分块喂入的结果必须与整块喂入完全一致
    p = FrameParser()
    for chunk in chunked(stream, size=5, seed=3):
        p.feed(chunk)
    check(
        "★ 分块喂入与整块喂入结果完全一致",
        (p.frames_ok, p.crc_errors, p.len_errors, p.dropped)
        == (whole.frames_ok, whole.crc_errors, whole.len_errors, whole.dropped),
        f"分块={p.frames_ok}/{p.crc_errors}/{p.len_errors}/{p.dropped} "
        f"整块={whole.frames_ok}/{whole.crc_errors}/{whole.len_errors}/{whole.dropped}",
    )

    # 极端情况：一次一个字节
    p = FrameParser()
    for i in range(len(stream)):
        p.feed(stream[i : i + 1])
    check("★ 逐字节喂入结果与整块一致",
          p.frames_ok == whole.frames_ok,
          f"逐字节={p.frames_ok} 整块={whole.frames_ok}")

    # 被接受的每一帧，其 raw 必须能自验 CRC 通过
    all_ok = True
    p = FrameParser()
    for frame in p.feed(stream):
        body_len = frame.raw[2]
        if crc16_modbus(frame.raw[2 : 4 + body_len]) != int.from_bytes(
                frame.raw[-2:], "little"):
            all_ok = False
    check("被接受的帧全部通过 CRC 复核", all_ok)


# ---------------------------------------------------------------- 10. 与 C 侧一致性

#: 匹配 `#define NAME 值`。值取到行尾或第一个 '/'（即注释之前）为止，
#: 所以带 /**< ... */ 尾注释的行也能正确取到值。
_C_MACRO_RE = re.compile(r"^#define\s+(\w+)\s+([^\n/]+)", re.M)


def _c_defines(src: str) -> dict[str, str]:
    """把一份 C 头文件里的 #define 抓成字典。函数式宏（带括号的）不收。"""
    return {name: val.strip() for name, val in _C_MACRO_RE.findall(src)}


def _expand_c_macro(name: str, src: str) -> int | None:
    """把 C 的 #define 表达式递归展开成整数，展开不了返回 None。

    只做宏名替换，不处理条件编译——够用即可，不追求在这里写个预处理器。
    展开后必须只剩数字和运算符才求值，避免把乱七八糟的东西 eval 掉。
    """
    defs = _c_defines(src)
    expr = defs.get(name)
    if expr is None:
        return None

    for _ in range(16):  # 上限防宏互相引用成环
        expanded = re.sub(r"\b[A-Za-z_]\w*\b",
                          lambda m: defs.get(m.group(0), m.group(0)), expr)
        if expanded == expr:
            break
        expr = expanded

    expr = re.sub(r"(?<=\d)[uUlL]+", "", expr)  # 去掉 2u / 8UL 之类的后缀
    if not re.fullmatch(r"[0-9+\-*/()\s]+", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307
    except Exception:
        return None




def test_c_side_consistency():
    """解析 App/app_protocol.c，核对它与 Python 侧没有漂移。

    ★ 这不是"跑通了 C 代码" —— 本机没有 C 编译器，跑不了。这是一次
      **静态比对**：把 C 源文件当文本读进来，检查表和常量。

    为什么值得做：CRC 表有 256 个常量，只要**手敲错一个**，现象就是
    "上位机一帧都收不到，但什么都没报错" —— 这类问题在没有逻辑分析仪
    的情况下几乎无法定位。让机器逐项比对，比人眼可靠。
    """
    c_path = Path(__file__).resolve().parent.parent / "App" / "app_protocol.c"
    h_path = Path(__file__).resolve().parent.parent / "App" / "app_config.h"

    if not c_path.exists():
        check("找到 C 侧源文件 app_protocol.c", False, f"不存在: {c_path}")
        return

    text = c_path.read_text(encoding="utf-8")
    cfg = h_path.read_text(encoding="utf-8") if h_path.exists() else ""

    # ---- CRC 表 ----
    m = re.search(r"s_crc16_table\[256\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        check("在 C 侧找到 s_crc16_table 定义", False, "正则没匹配上")
        return

    values = tuple(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1)))
    check("C 侧 CRC 表项数 = 256", len(values) == 256, f"实际 {len(values)} 项")

    if len(values) == 256:
        mismatches = [i for i, (a, b) in enumerate(zip(values, CRC16_TABLE)) if a != b]
        check(
            "★ C 侧 CRC 表与 Python 实现逐项一致",
            not mismatches,
            f"有 {len(mismatches)} 项不一致，首个在索引 {mismatches[0]}"
            f"（C=0x{values[mismatches[0]]:04X} Py=0x{CRC16_TABLE[mismatches[0]]:04X}）"
            if mismatches else "",
        )

    # ---- 帧结构常量 ----
    def c_define(name: str, src: str) -> int | None:
        mm = re.search(rf"#define\s+{name}\s+\(?([0-9xA-Fa-fu]+)\)?", src)
        if not mm:
            return None
        return int(mm.group(1).rstrip("uU"), 0)

    check("C 侧 FRAME_HEADER0 = 0xAA",
          c_define("FRAME_HEADER0", cfg) == HEADER[0],
          f"实际 {c_define('FRAME_HEADER0', cfg)}")
    check("C 侧 FRAME_HEADER1 = 0x55",
          c_define("FRAME_HEADER1", cfg) == HEADER[1],
          f"实际 {c_define('FRAME_HEADER1', cfg)}")
    check("C 侧 FRAME_DATA_LEN = 8",
          c_define("FRAME_DATA_LEN", cfg) == DATA_LEN,
          f"实际 {c_define('FRAME_DATA_LEN', cfg)}")
    # FRAME_TOTAL_LEN 是个宏表达式而不是字面量，要展开才能比对
    total = _expand_c_macro("FRAME_TOTAL_LEN", cfg)
    check("C 侧 FRAME_TOTAL_LEN 展开后 = 14", total == FRAME_LEN,
          f"展开得 {total}，期望 {FRAME_LEN}")

    # ---- frame_build 里的字段写入顺序 ----
    # C 侧按 temp→humi→light→pot 顺序写，且每个都是"先高字节"。
    # 若有人把某个通道的 (>>8) 和 (&0xFF) 写反，这里能发现。
    body = text[text.find("frame_build"):]
    order = re.findall(r"out\[n\+\+\]\s*=\s*\(uint8_t\)\(\(uint16_t\)s->(\w+)\s*(>>\s*8|&\s*0xFFu)\)",
                       body)
    check("C 侧 frame_build 字段顺序为 温度/湿度 各两字节大端",
          [name for name, _ in order][:4] == ["temp_x100", "temp_x100",
                                              "humi_x100", "humi_x100"],
          f"实际 {order[:4]}")
    check("C 侧每个通道都是先高字节后低字节",
          [op.replace(" ", "") for _, op in order][:4] == [">>8", "&0xFFu",
                                                           ">>8", "&0xFFu"],
          f"实际 {[op for _, op in order][:4]}")

    # ---- CRC 覆盖范围 ----
    check("C 侧 CRC 覆盖 LEN+SEQ+DATA（不含帧头）",
          "FRAME_META_LEN + FRAME_DATA_LEN" in text and "&out[2]" in text,
          "没找到 crc16_modbus(&out[2], FRAME_META_LEN + FRAME_DATA_LEN)")
    check("C 侧 CRC 低字节在前",
          re.search(r"out\[n\+\+\]\s*=\s*\(uint8_t\)\(crc\s*&\s*0xFFu\)", text)
          is not None,
          "没找到「先写 crc 低字节」")


# ---------------------------------------------------------------- main

def main() -> int:
    test_crc()
    test_build()
    test_roundtrip()
    test_streaming()
    test_bad_frames()
    test_resync()
    test_packet_loss()
    test_deterministic_mix()
    test_messy_and_chunking()
    test_c_side_consistency()
    return summarize()


if __name__ == "__main__":
    sys.exit(main())
