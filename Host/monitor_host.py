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
import threading
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

#: 中文字体候选，按优先级排列。后面的都是 Windows 自带，装了就一定找得到。
CJK_FONT_CANDIDATES = ("Microsoft YaHei", "SimHei", "SimSun", "Noto Sans CJK SC")


def setup_cjk_font() -> str | None:
    """给 matplotlib 挑一个含汉字字形的字体，返回选中的字体名。

    ★ 为什么必须做：matplotlib 默认的 DejaVu Sans **没有汉字字形**，
      不设这个的话曲线窗口里所有中文标签都会渲染成豆腐块（□□□）——
      标题、轴标签、图例全军覆没，录演示视频时是当场翻车级的难看。
      这个坑很隐蔽，因为**它只报警告不报错**，`--selftest` 也照样"通过"。

    返回 None 表示一个中文字体都没找到（此时图上的中文会是方块，
    调用方应提示用户）。
    """
    from matplotlib import font_manager, rcParams

    available = {f.name for f in font_manager.fontManager.ttflist}
    for name in CJK_FONT_CANDIDATES:
        if name in available:
            rcParams["font.sans-serif"] = [name, *rcParams["font.sans-serif"]]
            # ★ 负号也得管：中文字体里通常没有 U+2212（真减号），
            #   不管的话温度出现负值时，刻度上的负号会变成豆腐块。
            rcParams["axes.unicode_minus"] = False
            return name
    return None


# ---------------------------------------------------------------- 数据源

#: 判为"虚拟串口"的特征串（出现在 hwid 或描述里就跳过）。
#: 蓝牙/RFCOMM 虚拟口就是这一类。
_VIRTUAL_PORT_MARKERS = ("BTHENUM", "RFCOMM", "BLUETOOTH")


def _is_virtual_port(p) -> bool:
    blob = f"{p.hwid} {p.description}".upper()
    return any(m in blob for m in _VIRTUAL_PORT_MARKERS)


def _pick_port(candidates):
    """从候选里挑一个最像"真实 USB 转串口"的，返回 (port 或 None, 提示语 或 None)。

    ★★ 为什么不能直接取 candidates[0]：
       本机常驻两个**蓝牙虚拟串口 COM5/COM6**，而它们**排在 CH340 前面**。
       原来的写法就是 `port = candidates[0].device`，于是不指定 --port 时
       必然选中蓝牙口 —— 现象是"上位机一条数据都收不到"，
       而人第一反应会去怀疑**固件没在发**，排查方向从一开始就错了。

    真实 USB 转串口（CH340 / CP2102 / FT232…）的 hwid 里带 `USB VID:PID`。
    挑不到 USB 口时**返回 None 让调用方报错**，而不是退而求其次选蓝牙口 ——
    "默默地选错"比"明确地失败"坏得多。
    """
    usb = [p for p in candidates if not _is_virtual_port(p)]
    if not usb:
        return None, None
    note = None
    if len(usb) > 1:
        note = f"找到 {len(usb)} 个可用串口，挑了第一个；要换用 --port 指定"
    return usb[0].device, note


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
            tag = "   ← 蓝牙/虚拟口，跳过" if _is_virtual_port(p) else ""
            print(f"  {p.device}  {p.description}{tag}")

        port, note = _pick_port(candidates)
        if port is None:
            sys.exit(
                "只找到蓝牙/虚拟串口，没有 USB 转串口设备。\n"
                "  · CH340 插上了吗？驱动装了吗？\n"
                "  · 或用 --port 手动指定，例如  --port COM3"
            )
        if note:
            print(f"⚠ {note}")
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

    font = setup_cjk_font()
    if font is None:
        print("⚠ 系统里没找到中文字体，图上的中文会显示成方块（□□□）")
    else:
        print(f"绘图字体：{font}")

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

    # ★★ 采集必须在**后台线程**里做，动画回调绝不能自己去 next(gen)。
    #
    #   踩过的坑：原来 update() 里写的是
    #       for _ in range(64):
    #           for frame in parser.feed(next(gen, b"")):
    #   本意是"把积压的数据一次吃干净"。但串口那条生成器在没数据时
    #   **不会返回**——`ser.read()` 超时返回空、`if chunk:` 不成立、
    #   它就自己再循环一次，直到下一帧到达才 yield。1Hz 的数据率下，
    #   每次 next() 实测阻塞约 **1.14 秒**，64 次就是 **73 秒**。
    #   GUI 主线程被摁住 73 秒 → 窗口一直"未响应"（用户实测症状）。
    #
    #   改法：读的归读、画的归画。后台线程负责喂帧，回调只做非阻塞的取。
    #   deque 的 append / popleft 在 CPython 下是原子的，不需要额外加锁。
    pending: deque[Frame] = deque()

    def reader() -> None:
        try:
            # ★ 注意是 `in gen` 而不是 `in gen()` —— gen 到这里**已经是一个
            #   生成器对象**了（main 里是 make_source()() 调了两次）。
            #   多写一对括号会立刻 TypeError，而且是在子线程里，主线程只看到
            #   "窗口开着但曲线永远不动"。
            for chunk in gen:
                pending.extend(parser.feed(chunk))
        except Exception as exc:  # noqa: BLE001
            # 串口被拔掉之类。★ 必须把异常原文打出来：只说一句"读取线程退出了"
            # 等于没说，排查时只能靠猜（本项目在 #error 那条上踩过同样的坑）。
            print(f"\n⚠ 串口读取线程退出：{type(exc).__name__}: {exc}")
            print("  曲线不会再更新。若为串口错误，检查 USB 线/端口占用后重启本程序。")

    threading.Thread(target=reader, daemon=True, name="frame-reader").start()

    def update(_):
        # 非阻塞：只取后台线程已经攒下的帧，有多少取多少，绝不在这里等
        while pending:
            frame = pending.popleft()
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
