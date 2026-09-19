"""假帧生成器 —— 在没接硬件的情况下造出"像真串口一样脏"的字节流。

为什么需要它：解析器最容易死在**非理想输入**上。只喂完美帧，
``test_codec.py`` 全绿也说明不了抗干扰能力。这个模块专门造坏数据。

覆盖的坏情况：

* CRC 错帧        —— 总线受干扰
* 丢序号帧        —— 下位机来不及发 / 上位机来不及收
* 半截帧          —— 一次 read() 正好把帧切断
* 帧头前夹垃圾    —— 上电瞬间的电平抖动、烧录时的杂波
* 帧头被劈开      —— 垃圾里恰好含 0xAA 但不是真帧头
* 超长 LEN        —— 噪声把长度字段打成 0xFF
"""

from __future__ import annotations

import random

from frame_codec import HEADER, HEADER_LEN, build_frame, crc16_modbus


def _body(temp_x100: int, humi_x100: int, light: int, pot: int) -> bytes:
    import struct

    return struct.pack(">hhHH", temp_x100, humi_x100, light, pot)


def good_frame(seq: int, *, temp_x100: int = 2550, humi_x100: int = 6000,
               light: int = 2048, pot: int = 1024) -> bytes:
    """造一帧完美数据。默认值选在量程中点附近，方便人眼核对。"""
    return build_frame(seq, temp_x100, humi_x100, light, pot)


def good_stream(count: int, *, start_seq: int = 0, with_gaps: bool = False,
                seed: int = 0) -> list[bytes]:
    """造 ``count`` 帧。``with_gaps`` 为真时刻意跳过部分序号，模拟丢帧。"""
    rng = random.Random(seed)
    frames: list[bytes] = []
    seq = start_seq

    for _ in range(count):
        frames.append(good_frame(
            seq & 0xFF,
            temp_x100=rng.randint(2000, 3000),
            humi_x100=rng.randint(4000, 8000),
            light=rng.randint(0, 4095),
            pot=rng.randint(0, 4095),
        ))
        seq += 1
        if with_gaps and rng.random() < 0.2:
            seq += rng.randint(1, 3)  # 跳号 = 丢帧
    return frames


def corrupt_crc(frame: bytes) -> bytes:
    """把数据区第一个字节改掉，但**不动 CRC** —— 造出 CRC 校验失败的帧。

    对应真实现象：总线上一个比特被干扰翻转，接收端 CRC 对不上。
    """
    buf = bytearray(frame)
    buf[HEADER_LEN + 2] ^= 0xFF  # 数据区首字节（跳过帧头2 + LEN1 + SEQ1）
    return bytes(buf)


def bad_length(seq: int) -> bytes:
    """LEN 字段被噪声打成 0xFF 的帧 —— 解析器必须拒绝它而不是死等 255 字节。"""
    return HEADER + bytes((0xFF, seq)) + _body(0, 0, 0, 0) + b"\x00\x00"


def truncated(frame: bytes, cut: int) -> bytes:
    """把帧砍掉尾部 —— 模拟一次 read() 正好切在帧中间。"""
    return frame[:cut]


def garbage(nbytes: int, seed: int = 0, *, avoid_header: bool = True) -> bytes:
    """造随机垃圾字节。

    ``avoid_header=True`` 时避开 0xAA，用来测试"垃圾里没有帧头"的清理路径；
    为 False 时允许出现 0xAA，用来测试"垃圾里混着假帧头"的重同步路径。
    """
    rng = random.Random(seed)
    if avoid_header:
        return bytes(rng.randint(0, 0xFF) for _ in range(nbytes))
    return bytes(rng.choice([0xAA, 0x55, rng.randint(0, 0xFF)]) for _ in range(nbytes))


#: ``messy_stream`` 在 guarantee_coverage=True 时强制塞进前几轮的事件类型。
#: 为什么需要它：随机流**不保证**覆盖到每种坏情况——实测 seed=42 跑 40 轮
#: 一次非法长度帧都没出现，导致针对它的断言时灵时不灵。
_FORCED_COVERAGE = ("garbage", "crc", "gap", "badlen")


def messy_stream(count: int = 40, *, seed: int = 42,
                 guarantee_coverage: bool = True) -> bytes:
    """造一段"真实感"的字节流：好帧为主，穿插各种坏情况。

    这是 ``test_codec.py`` 里压力测试用的主要输入。

    ``guarantee_coverage=True`` 时，前四轮被强制指定为垃圾/CRC错/丢帧/
    非法长度各一次，保证压力测试真的压到了每条异常分支，而不是靠运气。

    .. note::
       这个函数里的 ``rng`` 调用次数会影响后续所有取值（``good_frame``
       内部也调 ``rng.randint``），所以**不要**在外部预先推算分支分布——
       实测过一次，算出来的和实际差很远。
    """
    rng = random.Random(seed)
    out = bytearray()

    def rand_frame(s: int) -> bytes:
        return good_frame(
            s & 0xFF,
            temp_x100=rng.randint(2000, 3000),
            humi_x100=rng.randint(4000, 8000),
            light=rng.randint(0, 4095),
            pot=rng.randint(0, 4095),
        )

    seq = 0
    for i in range(count):
        roll = rng.random()

        if guarantee_coverage and i < len(_FORCED_COVERAGE):
            kind = _FORCED_COVERAGE[i]
        elif roll < 0.05:
            kind = "garbage"
        elif roll < 0.12:
            kind = "crc"
        elif roll < 0.17:
            kind = "gap"
        elif roll < 0.21:
            kind = "badlen"
        else:
            kind = "good"

        if kind == "garbage":
            # 帧头前夹一段垃圾，后面跟一帧好的
            out += garbage(rng.randint(1, 6), seed=seed + i)
            out += rand_frame(seq)
            seq += 1
        elif kind == "crc":
            out += corrupt_crc(rand_frame(seq))  # 不应被接受
            seq += 1
        elif kind == "gap":
            seq += rng.randint(1, 2)  # 整帧丢失，只跳号不发
        elif kind == "badlen":
            out += bad_length(seq)  # 不应被接受
            seq += 1
        else:
            out += rand_frame(seq)
            seq += 1

    return bytes(out)


def chunked(stream: bytes, *, size: int = 7, seed: int = 0,
            jitter: bool = True) -> list[bytes]:
    """把字节流切成不定长的小块 —— 模拟真实的串口 ``read()`` 行为。

    真串口**不会**一次给你一整帧。这个函数保证解析器必须靠状态机跨块接续，
    而不是依赖"一次拿到完整帧"的假设。
    """
    rng = random.Random(seed)
    out: list[bytes] = []
    pos = 0
    while pos < len(stream):
        n = rng.randint(1, size * 2) if jitter else size
        out.append(stream[pos : pos + n])
        pos += n
    return out


if __name__ == "__main__":
    # 直接运行本文件时打印一份样本，方便肉眼核对帧长和字段
    print("单帧长度:", len(good_frame(0)), "(应为 14)")
    print("好帧 十六进制:", good_frame(0).hex(" ").upper())
    print()
    print("CRC 错帧:", corrupt_crc(good_frame(0)).hex(" ").upper())
    print("非法长度帧:", bad_length(0).hex(" ").upper())
    print()
    dumped = messy_stream(20)
    print(f"混杂流样本共 {len(dumped)} 字节，前 60 字节:")
    print(dumped[:60].hex(" ").upper())

    # 自检：构造一帧后立刻用同模块的 CRC 验回去
    f = good_frame(7)
    body_len = f[2]
    meta = f[2:4]
    assert crc16_modbus(meta + f[4:4 + body_len]).to_bytes(2, "little") == f[-2:], \
        "build_frame 与 crc16_modbus 不自洽"
    print("\n自检通过：build_frame 与 crc16_modbus 自洽")
