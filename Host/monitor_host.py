"""上位机：接收下位机上报的帧，实时绘图并统计链路质量。

用法::

    python monitor_host.py                     # 打开默认串口，控制台打印
    python monitor_host.py --port COM5         # 指定串口
    python monitor_host.py --plot              # 带实时曲线窗口
    python monitor_host.py --csv data.csv      # 同时存盘
    python monitor_host.py --selftest          # ★ 不接硬件，用模拟数据跑通整条链路
    python monitor_host.py --replay dump.bin   # 回放之前抓下来的原始字节流

``--selftest`` 和 ``--replay`` 都不需要串口、也不需要 pyserial，
用来在没有硬件的时候验证解析、统计、存盘这几条链路是否正常。

依赖：
    pip install pyserial        # 只有读真实串口时才需要
    matplotlib                  # 只有 --plot 时才需要（本机已装）
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from collections import deque
from pathlib import Path

# 允许从仓库任意位置运行本脚本
sys.path.insert(0, str(Path(__file__).resolve().parent))

from frame_codec import Frame, FrameParser  # noqa: E402

#: 曲线窗口保留的采样点数。1000 点 @1Hz ≈ 17 分钟
PLOT_HISTORY = 1000

#: 控制台统计的打印间隔（秒）
STATS_INTERVAL_S = 2.0


# ---------------------------------------------------------------- 数据源

def source_serial(port: str | None, baud: int):
    """从串口读字节。需要 pyserial。"""
    try:
        import serial  # noqa: PLC0415
    except ImportError:
        sys.exit(
            "没有安装 pyserial。\n"
            "  装一个：  pip install pyserial\n"
            "  或者不接硬件先跑通链路：python monitor_host.py --selftest"
        )

    if port is None:
        from serial.tools import list_ports  # noqa: PLC0415

        candidates = list(list_ports.comports())
        if not candidates:
            sys.exit("没找到任何串口。检查 CH340 驱动和 USB 线，或用 --port 手动指定。")
        print("可用串口：")
        for p in candidates:
            print(f"  {p.device}  {p.description}")
        port = candidates[0].device
        print(f"自动选用 {port}（用 --port 可以指定别的）")

    ser = serial.Serial(port, baud, timeout=0.2)
    print(f"已打开 {port} @ {baud}")

    def gen():
        while True:
            chunk = ser.read(256)
            if chunk:
                yield chunk

    return gen


def source_selftest(rate_hz: float = 1.0):
    """造模拟数据，用来在没有硬件时验证整条上位机链路。

    刻意混入 CRC 错帧和丢帧，这样控制台里的"丢包率""CRC 错"统计
    不会是恒零 —— 恒零的统计等于没验证。
    """
    import random

    from sim_frames import corrupt_crc, good_frame

    rng = random.Random(20260919)
    seq = 0

    def gen():
        nonlocal seq
        while True:
            roll = rng.random()
            if roll < 0.05:
                yield corrupt_crc(good_frame(seq & 0xFF))   # CRC 错帧
            elif roll < 0.08:
                pass                                        # 整帧丢失
            else:
                t = int(2550 + 300 * rng.uniform(-1, 1))
                h = int(6000 + 500 * rng.uniform(-1, 1))
                yield good_frame(seq & 0xFF, temp_x100=t, humi_x100=h,
                                 light=rng.randint(0, 4095),
                                 pot=rng.randint(0, 4095))
            seq += 1
            time.sleep(1.0 / rate_hz)

    return gen


def source_replay(path: Path):
    """回放一个原始字节流文件（比如用串口工具抓下来的 .bin）。"""
    data = path.read_bytes()
    print(f"回放 {path}，共 {len(data)} 字节")

    def gen():
        # 按 64 字节切块，模拟串口的分包节奏
        for i in range(0, len(data), 64):
            yield data[i:i + 64]
            time.sleep(0.005)

    return gen


# ---------------------------------------------------------------- 输出

class CsvSink:
    """把解析出来的帧写进 CSV。"""

    def __init__(self, path: Path):
        self._f = path.open("w", newline="", encoding="utf-8")
        self._w = csv.writer(self._f)
        self._w.writerow(["seq", "temp_c", "humidity", "light", "pot"])
        self._f.flush()
        print(f"数据将写入 {path}")

    def write(self, frame: Frame) -> None:
        self._w.writerow([frame.seq, f"{frame.temp_c:.2f}",
                          f"{frame.humidity:.2f}", frame.light, frame.pot])
        # ★ 每条都 flush。数据率只有 1Hz，这点开销可以忽略；
        #   换来的是**进程被强杀（Ctrl+C 之外的 kill、崩溃、拔电）也不丢已收到的数据**。
        #   实测踩过：只靠 close() 落盘的话，被 kill 时连表头都留不下来。
        self._f.flush()

    def close(self) -> None:
        self._f.close()


def plot_loop(gen, parser: FrameParser, sink: CsvSink | None, csv_path: Path | None):
    """带实时曲线的模式。"""
    try:
        import matplotlib.animation as animation  # noqa: PLC0415
        import matplotlib.pyplot as plt  # noqa: PLC0415
    except ImportError:
        sys.exit("没有安装 matplotlib，去掉 --plot 用控制台模式，或者 pip install matplotlib")

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    fig.suptitle("STM32 FreeRTOS 多任务环境监测终端")

    hist_t: deque[float] = deque(maxlen=PLOT_HISTORY)
    hist_h: deque[float] = deque(maxlen=PLOT_HISTORY)
    hist_l: deque[int] = deque(maxlen=PLOT_HISTORY)

    line_t, = ax1.plot([], [], "r-", label="温度 (℃)")
    line_h, = ax1.plot([], [], "b-", label="湿度 (%RH)")
    ax1.set_ylabel("温度 / 湿度")
    ax1.legend(loc="upper left")
    ax1.grid(True, alpha=0.3)

    line_l, = ax2.plot([], [], "g-", label="光照 (ADC)")
    ax2.set_ylabel("光照原始值")
    ax2.set_xlabel("采样点")
    ax2.legend(loc="upper left")
    ax2.grid(True, alpha=0.3)

    def update(_):
        # 每帧尽量把所有积压的数据都吃掉，避免曲线落后于现实
        for _ in range(64):
            for frame in parser.feed(next(gen, b"")):
                hist_t.append(frame.temp_c)
                hist_h.append(frame.humidity)
                hist_l.append(frame.light)
                if sink:
                    sink.write(frame)

        n = len(hist_t)
        if n:
            xs = range(n)
            line_t.set_data(xs, hist_t)
            line_h.set_data(xs, hist_h)
            line_l.set_data(xs, hist_l)
            ax1.relim()
            ax1.autoscale_view()
            ax2.relim()
            ax2.autoscale_view()

        ax1.set_title(parser.stats_line(), fontsize=9)
        return line_t, line_h, line_l

    # 没有 blit，因为标题每帧都在变
    _anim = animation.FuncAnimation(fig, update, interval=200, cache_frame_data=False)
    plt.tight_layout()
    plt.show()


def console_loop(gen, parser: FrameParser, sink: CsvSink | None):
    """控制台模式：不依赖 matplotlib，适合 SSH / 无图形环境。"""
    last_stats = time.monotonic()
    print("开始接收（Ctrl+C 退出）...\n")

    try:
        while True:
            chunk = next(gen, b"")
            for frame in parser.feed(chunk):
                print(f"  {frame}")
                if sink:
                    sink.write(frame)

            now = time.monotonic()
            if now - last_stats >= STATS_INTERVAL_S:
                last_stats = now
                print(f"  ── {parser.stats_line()}")
    except KeyboardInterrupt:
        print("\n已停止。")
        print(f"最终统计：{parser.stats_line()}")


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(
        description="STM32 多任务环境监测终端 · Python 上位机",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--port", help="串口号，如 COM5。不指定则自动选第一个")
    ap.add_argument("--baud", type=int, default=115200, help="波特率（默认 115200）")
    ap.add_argument("--plot", action="store_true", help="显示实时曲线窗口")
    ap.add_argument("--csv", type=Path, help="把数据存成 CSV")
    ap.add_argument("--replay", type=Path, help="回放一个原始字节流文件")
    ap.add_argument("--selftest", action="store_true",
                    help="★ 不接硬件，用模拟数据验证整条上位机链路")
    ap.add_argument("--show-crc-table", action="store_true",
                    help="打印 CRC16 表（供下位机 C 实现核对）")
    args = ap.parse_args()

    if args.show_crc_table:
        from frame_codec import CRC16_TABLE

        for i in range(0, 256, 8):
            print("    " + ", ".join(f"0x{v:04X}" for v in CRC16_TABLE[i:i + 8]) + ",")
        return 0

    if args.selftest:
        make_source = source_selftest
    elif args.replay:
        make_source = lambda: source_replay(args.replay)  # noqa: E731
    else:
        make_source = lambda: source_serial(args.port, args.baud)  # noqa: E731

    parser = FrameParser()
    sink = CsvSink(args.csv) if args.csv else None

    try:
        gen = make_source()()
        if args.plot:
            plot_loop(gen, parser, sink, args.csv)
        else:
            console_loop(gen, parser, sink)
    finally:
        if sink:
            sink.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
