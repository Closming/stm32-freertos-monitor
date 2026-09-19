"""串口帧编解码 —— 与下位机 App/app_protocol.c 严格对称。

帧格式（共 14 字节，LEN = 8 时）::

    偏移  字段     长度  说明
     0    0xAA     1     帧头 1
     1    0x55     1     帧头 2
     2    LEN      1     数据区长度
     3    SEQ      1     帧序号 0~255 循环
     4-5  温度     2     int16 大端，×100
     6-7  湿度     2     int16 大端，×100
     8-9  光照     2     uint16 大端，ADC 原始值
    10-11 电位器   2     uint16 大端，ADC 原始值
    12-13 CRC16    2     Modbus CRC16，低字节在前

设计取舍（面试可讲）：

* **数据区用大端**：网络字节序，串口抓包时人眼可读。
* **CRC 低字节在前**：与 Modbus-RTU 一致（Modbus 串行链路上 CRC 就是低字节先发）。
* **LEN 独立成字段**：日后加通道（比如报警状态位）只需扩 LEN，
  上位机按 LEN 跳过未知尾部即可，老解析器不会崩。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

# ---------------------------------------------------------------- 常量

HEADER = b"\xAA\x55"
HEADER_LEN = len(HEADER)
# 头部之后还有 LEN(1) + SEQ(1)，再加数据区和 CRC(2)
META_LEN = 2  # LEN + SEQ
CRC_LEN = 2

#: 当前固件实际发送的数据区长度：温度/湿度/光照/电位器 各 2 字节
DATA_LEN = 8
#: 单帧总长
FRAME_LEN = HEADER_LEN + META_LEN + DATA_LEN + CRC_LEN  # = 14

#: 解析器允许的最大数据区长度。留出扩展余量，同时防止 LEN 字段
#: 被噪声打成 0xFF 后死等 255 字节。
MAX_DATA_LEN = 64

#: 四个通道的原始值缩放系数（下位机 ×100 后发整数，避免浮点传输）
SCALE = 100.0


# ---------------------------------------------------------------- CRC16

def _build_crc_table() -> tuple[int, ...]:
    """生成 CRC16-Modbus 查表（多项式 0xA001，即 0x8005 的反射形式）。"""
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
        table.append(crc)
    return tuple(table)


#: 256 项查表。与下位机 App/app_protocol.c 里的 s_crc16_table 必须逐字节相同。
CRC16_TABLE = _build_crc_table()


def crc16_modbus(data: bytes, init: int = 0xFFFF) -> int:
    """算 CRC16-Modbus。``init`` 默认 0xFFFF。

    查表法一次处理一个字节，比逐位法快 8 倍；表在导入时算一次。
    """
    crc = init
    for byte in data:
        crc = (crc >> 8) ^ CRC16_TABLE[(crc ^ byte) & 0xFF]
    return crc


# ---------------------------------------------------------------- 帧结构

@dataclass(frozen=True, slots=True)
class Frame:
    """一帧解出来的内容。"""

    seq: int
    temp_c: float       # 摄氏度
    humidity: float     # %RH
    light: int          # ADC 原始值 0~4095
    pot: int            # ADC 原始值 0~4095
    raw: bytes          # 原始字节，便于存盘复现

    def __str__(self) -> str:
        return (
            f"seq={self.seq:3d}  {self.temp_c:6.2f}℃  "
            f"{self.humidity:6.2f}%RH  light={self.light:4d}  pot={self.pot:4d}"
        )


def build_frame(
    seq: int,
    temp_x100: int,
    humi_x100: int,
    light: int,
    pot: int,
) -> bytes:
    """按协议打包一帧。

    参数是**已放大 100 倍的整数**（温度/湿度），量程与 int16 一致：
    ``temp_x100=-4000`` 表示 -40.00℃。

    这个函数既用于下位机的对照实现，也被 sim_frames 用来造测试数据。
    """
    if not 0 <= seq <= 0xFF:
        raise ValueError(f"seq 必须是 0~255，得到 {seq}")

    body = struct.pack(">hhHH", temp_x100, humi_x100, light, pot)
    meta = bytes((len(body), seq))
    checksum = crc16_modbus(meta + body)
    # CRC 低字节在前（Modbus 惯例）
    return HEADER + meta + body + struct.pack("<H", checksum)


# ---------------------------------------------------------------- 解析器

class FrameParser:
    """带**失败回退**的帧同步解析器。

    核心难点不是"怎么解一帧"，而是"帧头认错了怎么办"。

    串口上电瞬间、烧录时的杂波、总线干扰，都可能让数据里凭空出现一段
    ``AA 55``。如果解析器一看到 ``AA 55`` 就把后面固定长度当成帧体吃掉，
    那么**真帧的帧头如果正好落在这段被吃掉的区间里，整帧就报废了**。
    实测确实会：垃圾 ``AA 55 56 ... AA 55`` 后面接真帧时，真帧被完整丢掉。

    所以这里的关键设计是：**校验失败时只后退一个字节重新找帧头，绝不整段丢弃。**

    ======================  ==========================================
    失败情形                 处理
    ======================  ==========================================
    找不到帧头               丢掉垃圾，只保留末尾一个 ``0xAA``
                             （它可能是被 read() 切断的帧头前半）
    LEN 字段非法             ``len_errors++``，后退 1 字节重扫
    CRC 对不上               ``crc_errors++``，后退 1 字节重扫
    ======================  ==========================================

    "后退 1 字节"而不是"整段重置"，是为了让落在误判区间里的真帧头
    还有被重新发现的机会。
    """

    def __init__(self) -> None:
        self._buf = bytearray()

        # 统计量 —— 上位机界面上要显示的就是这些
        self.frames_ok = 0        # 校验通过
        self.crc_errors = 0       # 帧头对上了但 CRC 不对（总线干扰/丢字节）
        self.len_errors = 0       # LEN 字段非法
        self.dropped = 0          # 靠 SEQ 跳变推出来的丢帧数
        self.discarded = 0        # 为重新同步而扔掉的垃圾字节

        self._last_seq: int | None = None

    # ---------------------------------------------------------- 内部

    def _drop_front(self, n: int) -> None:
        """丢掉缓冲区开头的 n 个字节，计入 discarded。"""
        if n > 0:
            self.discarded += n
            del self._buf[:n]

    def _emit(self, candidate: bytes, data_len: int) -> Frame:
        """把一段已通过 CRC 的字节解成 Frame 并更新 SEQ 统计。"""
        seq = candidate[3]
        body = candidate[4 : 4 + data_len]
        got_crc = int.from_bytes(candidate[4 + data_len :], "little")

        # 数据区不足 8 字节解不出四通道；多出来的字节属于未来扩展，忽略
        temp_x100, humi_x100, light, pot = struct.unpack(">hhHH", body[:DATA_LEN])

        if self._last_seq is not None:
            self.dropped += (seq - self._last_seq - 1) & 0xFF
        self._last_seq = seq
        self.frames_ok += 1

        return Frame(
            seq=seq,
            temp_c=temp_x100 / SCALE,
            humidity=humi_x100 / SCALE,
            light=light,
            pot=pot,
            raw=candidate[: 4 + data_len + CRC_LEN],
        )

    # ---------------------------------------------------------- 对外

    def feed(self, chunk: bytes) -> list[Frame]:
        """喂入一段字节流，返回本次能解析出的所有完整帧。

        可以任意切分输入：一次一字节、一次半帧、一次十帧，结果都一样。
        """
        self._buf += chunk
        out: list[Frame] = []

        while True:
            start = self._buf.find(HEADER)

            if start < 0:
                # 一个帧头都没有。末尾若是单独的 0xAA，它可能是被切断的帧头，
                # 留着等下一批数据；其余全是垃圾。
                keep = 1 if self._buf and self._buf[-1] == HEADER[0] else 0
                self._drop_front(len(self._buf) - keep)
                break

            if start > 0:
                self._drop_front(start)

            if len(self._buf) < HEADER_LEN + META_LEN:
                break  # 还没拿到 LEN 字段，等更多数据

            data_len = self._buf[2]

            if not 1 <= data_len <= MAX_DATA_LEN:
                self.len_errors += 1
                self._drop_front(1)  # ★ 后退 1 字节重扫，不整段丢弃
                continue

            total = HEADER_LEN + META_LEN + data_len + CRC_LEN
            if len(self._buf) < total:
                break  # 帧还没收全，等

            candidate = bytes(self._buf[:total])
            expected = crc16_modbus(candidate[2 : 4 + data_len])
            actual = int.from_bytes(candidate[4 + data_len : total], "little")

            if expected != actual:
                self.crc_errors += 1
                self._drop_front(1)  # ★ 同样只后退 1 字节
                continue

            out.append(self._emit(candidate, data_len))
            self._drop_front(total)

        return out

    # ------------------------------------------------------------ 统计

    @property
    def packet_loss(self) -> float:
        """丢包率 = 丢帧 / (收到 + 丢)，还没有数据时返回 0。"""
        total = self.frames_ok + self.dropped
        return self.dropped / total if total else 0.0

    def stats_line(self) -> str:
        return (
            f"收 {self.frames_ok} 帧 | 丢 {self.dropped} 帧 "
            f"({self.packet_loss * 100:.1f}%) | CRC 错 {self.crc_errors} "
            f"| 长度错 {self.len_errors} | 丢弃字节 {self.discarded}"
        )
