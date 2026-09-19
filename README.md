# STM32 + FreeRTOS 多任务环境监测终端

基于 STM32F103C8T6 的实时环境监测系统。下位机以六个 FreeRTOS 任务并发采集
温湿度与两路模拟量，经自定义串口协议上报；Python 上位机实时解析、绘图并统计链路质量。

**这个项目的重点不在"读了多少个传感器"，而在三个具体的工程问题：**

1. **一条 I²C 总线上挂了两个器件，由两个不同优先级的任务驱动** —— 怎么保证不打架
2. **六个任务的周期相差 20 倍（50ms ~ 1000ms）** —— 怎么划优先级和栈
3. **慢操作（读传感器、刷屏）会拖垮快周期任务** —— 怎么把持锁时间压下来

---

## 当前状态

> ⚠️ **诚实起见，先把验证到什么程度写清楚。**

| 部分 | 状态 |
|---|---|
| Python 协议层（编解码 / CRC / 状态机解析） | ✅ **已测试**，`python Host/test_codec.py` 49 项全过 |
| Python 上位机（解析 / 统计 / CSV） | ✅ **已测试**，`--selftest` 离线跑通 |
| C 侧全部源码（App 目录） | ✅ **编译验证**，`bash Tools/check_app.sh` 8 个文件全过 |
| 协议在 C 与 Python 两侧的一致性 | ✅ **静态比对通过**（CRC 表 256 项逐项一致） |
| **整个 Keil 工程链接通过** | ✅ **`0 Error(s), 0 Warning(s)`**，`Code=26104 + RO=1648 + RW=236` ≈ **27.3 KB Flash**，`ZI=16564` ≈ **16.8 KB RAM (82%)** |
| 功能完整性全盘审查 | ✅ 查了「配置 ↔ 代码 ↔ 设计目标」三方，**修掉 5 个问题**（含 1 个致命的），见下方「已知局限」 |
| 硬件驱动实际跑通 | ⏳ **未验证** —— 硬件到货后需要实测调试 |
| I²C 总线冲突的复现与解决 | ⏳ **未验证** —— 见下方「遇到的问题与解决」 |

**没有在真机上跑过的部分，这里不会写成已完成。** 编译通过不等于运行正确。

---

## 系统架构

```
┌──────────────────────────── STM32F103C8T6 ────────────────────────────┐
│                                                                        │
│  硬件层                                                                │
│    ADC1 (扫描 + DMA 循环搬运)  →  光敏电阻 / 电位器  (PA1 / PA2)       │
│    GPIO 软件模拟 I²C (PB6/PB7) →  SHT30 (0x44)  ┐                     │
│                                →  SSD1306 (0x3C) ┘ ★ 共用总线          │
│    USART1 (TX + DMA)           →  CH340 → PC   (PA9 / PA10)            │
│    EXTI (PA3)                  →  按键中断                             │
│    IWDG                        →  独立看门狗                            │
│                                                                        │
│  FreeRTOS 层（CMSIS-RTOS v2 接口）                                     │
│                                                                        │
│   ┌──────────────┐  队列    ┌───────────────┐  队列   ┌────────────┐  │
│   │ TaskAcquire  │ ───────→ │  TaskProcess  │ ──────→ │ TaskDisplay│  │
│   │  50ms  优先级最高       │  事件驱动      │         │  500ms     │  │
│   └──────┬───────┘          │  越限判定      │         └─────┬──────┘  │
│          │ 置 ADC 就绪位     └───────┬───────┘               │         │
│          ↓                          │ 队列                  │         │
│   ┌──────────────┐                  ↓                ┌──────┴─────┐   │
│   │ TaskSensor   │          ┌──────────────┐         │  I²C 互斥锁 │   │
│   │  1000ms      │          │  TaskComm    │         │  Mutex     │   │
│   │  读 SHT30    │          │  CRC16+DMA   │         │ ★ 优先级继承│   │
│   └──────┬───────┘          └──────────────┘         │ ★ 递归锁    │   │
│          │ 置 SHT30 就绪位                             └──────┬─────┘   │
│          └──────────────→ 事件组（多源同步）                   │         │
│                                                               │         │
│   ┌──────────────┐  喂狗 + 心跳巡检 + 按键消音        共用总线 ┘         │
│   │ TaskMonitor  │ ←──── 任务通知（来自 EXTI 中断）                    │
│   │  500ms       │                                                     │
│   └──────────────┘                                                     │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ USART1 115200 8N1
                                    ↓
                    ┌───────────────────────────────┐
                    │  Python 上位机 (pyserial)      │
                    │  · 帧同步状态机 + CRC 校验     │
                    │  · matplotlib 实时曲线         │
                    │  · 丢包率 / CRC 错统计         │
                    │  · CSV 数据落盘                │
                    └───────────────────────────────┘
```

---

## 任务划分

| 任务 | CMSIS-RTOS2 优先级 | 栈 | 周期 | 职责 |
|---|---|---|---|---|
| `TaskAcquire` | `osPriorityRealtime` | 128 字 | 50 ms | 读 DMA 缓冲、8 点滑动平均、发队列 |
| `TaskSensor` | `osPriorityHigh` | 256 字 | 1000 ms | 读 SHT30（两次 I²C 事务 + 20ms 等待） |
| `TaskProcess` | `osPriorityAboveNormal` | 192 字 | 事件驱动 | 等两路数据就绪 → 越限判定 → 分发 |
| `TaskMonitor` | `osPriorityAboveNormal` | 128 字 | 500 ms | 喂 IWDG、查心跳、驱动蜂鸣器、响应按键 |
| `TaskComm` | `osPriorityNormal` | 256 字 | 事件驱动 | 打包 + CRC16 + UART DMA 发送 |
| `TaskDisplay` | `osPriorityBelowNormal` | 256 字 | 500 ms | 刷 OLED（**持总线锁最久，所以优先级最低**） |

### 两个刻意的设计决定

**为什么采集任务优先级最高？**
50ms 是硬周期，它一旦被打断太久，滑动平均窗口的采样间隔就不均匀，
滤波结果会失真。整条链路都以它为节拍。

**为什么显示任务优先级最低？**
它持 I²C 总线锁的时间最长（刷一屏约 150ms，拆成 4 段）。
优先级给高了，传感器读数会被堵在后面 —— 那就违背了加锁的初衷。

> ⚠️ **CMSIS-RTOS2 的优先级是 `osPriority_t` 枚举，不是数字。**
> 映射到 FreeRTOS（`configMAX_PRIORITIES = 7`）后实际只有 5 个可用档位，
> 所以上表六个任务里有四个共享档位。排序依据是「实时性要求 + 持锁时长」。

### 任务间通信的三种机制，各用在哪

| 机制 | 用在哪 | 为什么是它 |
|---|---|---|
| **队列** | 采集→处理→显示/上报 | 传**数据**，有缓冲，生产者不会阻塞 |
| **事件组** | 处理任务等两路数据到齐 | **多源同步**：`osFlagsWaitAll` 等两位同时置位，天然是"到齐一次处理一次" |
| **任务通知** | EXTI 中断 → 监控任务 | 中断里开销最小，不需要预先创建任何对象 |

> **为什么显示和上报要用两条队列？** 队列是点对点的，同一条队列里的每条消息
> 只会被一个消费者取走。两个消费者共用一个队列会各拿到一半 —— 那是错的。

---

## 串口协议

```
偏移  字段     长度  说明
 0    0xAA     1     帧头 1
 1    0x55     1     帧头 2
 2    LEN      1     数据区长度（当前 = 8）
 3    SEQ      1     帧序号 0~255 循环
 4-5  温度     2     int16 大端，×100（-4000 = -40.00℃）
 6-7  湿度     2     int16 大端，×100
 8-9  光照     2     uint16 大端，ADC 原始值
10-11 电位器   2     uint16 大端，ADC 原始值
12-13 CRC16    2     Modbus CRC16，低字节在前，覆盖 LEN+SEQ+DATA
```

单帧定长 14 字节。

**三个设计取舍：**

- **双帧头 `0xAA 0x55`**：降低误同步概率。单字节帧头在噪声里被误认的概率太高。
- **数据区用大端、CRC 低字节在前**：这是 **Modbus-RTU 的惯例**（串行链路上 CRC 就是低字节先发），
  不是随手定的。和 Modbus 打交道多的岗位一眼能看出这是有意为之。
- **LEN 独立成字段**：日后加通道只需扩 LEN，上位机按 LEN 跳过未知尾部即可，老解析器不会崩。

### 上位机的帧同步

解析用的是**显式状态机**，不是 `buf.find(b'\xAA\x55')` 那种一次性切片。
真实串口是流式的：一次 `read()` 可能拿到半帧，也可能一次拿到三帧半，
纯靠切片会写出一堆边界条件。

**★ 关键设计：校验失败时只后退一个字节重新找帧头，绝不整段丢弃。**

这一条不是想出来的，是**测出来的**。最初的实现在 CRC 校验失败时整段重置，
结果在"垃圾字节里恰好出现 `AA 55`"的场景下会丢掉真帧：

```
垃圾: ... AA 55 56 ...          ← 假的帧头
真帧: AA 55 08 ...              ← 真的帧头

假帧头被当成帧头 → 真帧的头两字节被当成 LEN 字段吃掉 → 整帧报废
```

后退一字节重扫之后，落在误判区间里的真帧头还有被重新发现的机会。
`Host/test_codec.py` 里有一条测试专门盯着这个场景。

---

## 硬件清单与接线

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控板 | STM32F103C8T6 最小系统板（"蓝 pill"） | |
| 下载器 | ST-Link V2 | 接 4 pin SWD 排针 |
| **USB-TTL** | **CH340 模块** | ★ 必需。蓝 pill 的 USB 口是原生 USB 外设，不能当串口用 |
| 显示屏 | 0.96" OLED，SSD1306，**I²C** | 地址 `0x3C` |
| 温湿度 | SHT30 模块，**I²C** | 地址 `0x44` |
| 光照 | GL5528 光敏电阻 + 10kΩ 电阻分压 | |
| 模拟量 | 10kΩ 电位器 | 手动可调，方便演示 |
| 其他 | 面包板、杜邦线、蜂鸣器、按键 | |

### 引脚分配

| 用途 | 引脚 | 说明 |
|---|---|---|
| 软件 I²C SCL | **PB6** | 开漏输出 |
| 软件 I²C SDA | **PB7** | 开漏输出 |
| USART1 TX / RX | PA9 / PA10 | 接 CH340：模块 TX→PA10，模块 RX→PA9，**GND 必须共地** |
| ADC 光敏 | **PA1** | ADC_IN1 |
| ADC 电位器 | **PA2** | ADC_IN2 |
| 按键 EXTI | **PA3** | 下降沿触发 |
| 蜂鸣器 | PB0 | 推挽输出 |
| 板载 LED | PC13 | **低电平点亮** |
| SWD | PA13 / PA14 | |

> **★ 为什么光敏不接 PA0：** 蓝 pill 的**板载按键物理接在 PA0**。把 PA0 当 ADC 输入时，
> 误按板载按键会灌入 3.3V，读数直接饱和到 4095 —— 不烧，但调光敏的时候会怀疑人生。
> 所以两路 ADC 全部挪开，PA0 空置。

> **⚠️ 分压只能接 3.3V，绝不能接 5V。** ADC 输入不得超过 VDDA，接 5V 会打坏引脚。

> **⚠️ I²C 需要上拉电阻。** 大多数 SHT30 / OLED 模块板载已有，但如果没有，
> 总线会完全不通（表现为读不到任何 ACK）。

---

## 构建与运行

```bash
git clone https://github.com/Closming/stm32-freertos-monitor.git
```

### 下位机

```
Keil MDK-ARM 打开 MDK-ARM/MonitorTerminal.uvprojx → F7 编译 → F8 烧录
```

命令行编译（不用开 GUI）：

```bash
"G:/keil/UV4/UV4.exe" -b "MDK-ARM/MonitorTerminal.uvprojx" -j0 -o build.log
echo $?    # 0 = 无错，1 = 有警告，2 = 有错误
```

### 上位机

```bash
cd Host
pip install pyserial                    # 只有读真实串口才需要

python monitor_host.py --selftest       # ★ 不接硬件，验证整条上位机链路
python monitor_host.py --port COM5      # 读真实串口
python monitor_host.py --port COM5 --plot --csv data.csv
```

### 测试

```bash
cd Host && python test_codec.py         # 协议层 49 项测试（不需要硬件）
bash Tools/check_app.sh                 # App 目录全部源文件的编译检查
```

---

## 目录结构

```
├── App/              ← 本项目代码（CubeMX 不管理这个目录）
│   ├── app_config.h        引脚 / 阈值 / 任务参数，全部集中在这里
│   ├── app_i2c.c/.h        ★ GPIO 软件模拟 I²C + 总线互斥锁
│   ├── app_sht30.c/.h      SHT30 驱动（含 CRC8）
│   ├── app_ssd1306.c/.h    OLED 驱动（自写最小实现，走总线锁）
│   ├── app_font.c/.h       5×7 字库（由 Tools/gen_font.py 生成）
│   ├── app_adc.c/.h        ADC + DMA + 滑动平均
│   ├── app_protocol.c/.h   帧打包 + CRC16-Modbus
│   ├── app_uart.c/.h       printf 重定向 + DMA 发帧 + 任务安全日志
│   └── app_tasks.c/.h      六个任务 + 队列 / 事件组 / 任务通知
├── Host/             ← Python 上位机
│   ├── frame_codec.py      帧编解码 + CRC16（与 C 侧严格对称）
│   ├── sim_frames.py       假帧生成器（错帧 / 丢帧 / 半帧 / 垃圾字节）
│   ├── test_codec.py       ★ 离线自测，49 项
│   └── monitor_host.py     主程序
├── Tools/            ← 开发辅助
│   ├── check_app.sh        App 源文件的编译检查（用 armcc，不需要硬件）
│   ├── syntax_stub/        编译检查用的桩头文件
│   └── gen_font.py         字库生成器（可渲染回点阵图核对）
├── Src/ Inc/ Drivers/ MDK-ARM/   ← CubeMX 生成，不手工改
└── docs/              ← 截图、波形图
```

---

## ⚠️ 本工程对 CubeMX 生成代码做过的改动（重新 GENERATE 会全部丢失）

**这一节很重要。** 项目用的是 CMSIS-RTOS **V2**，但 **CubeMX 不为 STM32F1 提供 V2 选项**
（见下），所以生成出来的代码是按 V1 规则写的，之后做了手工改造。

> **如果在 CubeMX 里重新 `GENERATE CODE`，下面这些改动会全部被覆盖，工程将无法编译。**
> 请照着本表逐项恢复。

### 背景：CubeMX 为什么不给 F1 用 CMSIS_V2

`CMSIS_V2` 那个选项在 CubeMX 里是**灰色不可选**的。读它自己的定义文件
（`db/mcu/IP/FREERTOS-v8.0.0_Cube_Modes.xml`）可以看到：

```xml
<Mode Name="CMSIS_V2">
    <Condition Expression="STM32F4|STM32F7|STM32G0|STM32G4|STM32H7|STM32L1|STM32L4|STM32MP1|STM32WB"
               Diagnostic="Available for F4,F7,G0,G4,H7,L1,L4,MP1 and WB series"/>
</Mode>
```

**`STM32F1` 不在列表里**，旁边还有注释 `<!-- "CMSIS_V2" being delivered series after series -->`
—— 是逐系列放开的，F1 还没轮到。

但 **V2 的源码本来就在固件包里**：
`STM32Cube_FW_F1_V1.8.4\Middlewares\Third_Party\FreeRTOS\Source\CMSIS_RTOS_V2\`

所以做法是：**让 CubeMX 按 CMSIS_V1 生成脚手架，再手工把封装层换成 V2。**

### 改动清单

| # | 文件 | 改了什么 | 为什么 |
|---|---|---|---|
| 1 | `Middlewares/.../CMSIS_RTOS_V2/` | **新增目录**，放入 `cmsis_os2.c/.h`、`cmsis_os.h`、`cmsis_os1.c` | CubeMX 选了 V1 就不会复制 V2 的文件 |
| 2 | `MDK-ARM/*.uvprojx` | Keil 的 `Middlewares/FreeRTOS` 分组里：`cmsis_os.c` → **`cmsis_os2.c` + `cmsis_os1.c`** | 换成 V2 实现 |
| 3 | `MDK-ARM/*.uvprojx` | IncludePath：`Source/CMSIS_RTOS` → **`Source/CMSIS_RTOS_V2`** | 让 `#include "cmsis_os.h"` 解析到 V2 那套 |
| 4 | `MDK-ARM/*.uvprojx` | IncludePath 追加 **`../App`** | `Src/freertos.c` 要 `#include "app_tasks.h"` |
| 5 | `Inc/FreeRTOSConfig.h` | 补 `configUSE_RECURSIVE_MUTEXES` / `configUSE_COUNTING_SEMAPHORES` / `configUSE_TIMERS` / `configUSE_TASK_NOTIFICATIONS` / 三个 `configTIMER_*` / `configUSE_TRACE_FACILITY` / 十一个 `INCLUDE_*` | **V2 封装层会调用这些 API 且外面没有 `#if` 守卫**，不开就是 `L6218E: Undefined symbol` |
| 6 | `Inc/FreeRTOSConfig.h` | `configTOTAL_HEAP_SIZE` 10240 → **12288** | 六任务 + 空闲任务 + 定时器任务的堆需求 |
| 7 | `Src/freertos.c` | `USER CODE` 区加 `#include "app_tasks.h"` 和 `app_init()` | 本项目代码的入口 |
| 8 | `Src/freertos.c` | **删掉 defaultTask**（句柄、`osThreadDef`、`osThreadCreate`、`StartDefaultTask` 函数体） | CubeMX 按 **V1** 规则生成的 `osThreadDef(name, thread, prio, inst, stack)` 是 **5 参数**，而 V2 兼容层是 **4 参数**（V2 把入口函数延后到 `osThreadNew` 才传）→ 直接编译不过。它本来也只是个空循环 |
| 9 | `Src/main.c` | `USER CODE BEGIN 2` 里加 **`osKernelInitialize()`** | ★★ **致命**：V1 没有这个 API，缺了它 `KernelState` 停在 `osKernelInactive` → `osKernelStart()` 直接返回错误 → **`vTaskStartScheduler()` 从不执行，六个任务一个都不跑** |
| 10 | `Src/iwdg.c` | 分频 `IWDG_PRESCALER_8` → **`_64`**，重装载 `1250` → **`1249`** | 原配置实际超时只有 **250ms**（`IWDG_PRESCALER_8` 是分频系数 8，不是 2⁸），而喂狗周期 500ms → **板子会不停复位** |
| 11 | `Src/stm32f1xx_it.c` | 加 `EXTI3_IRQHandler` | CubeMX 没生成（EXTI3 的 NVIC 当时没使能） |
| 12 | `Src/gpio.c` | `MX_GPIO_Init()` 末尾加 `HAL_NVIC_SetPriority/EnableIRQ(EXTI3_IRQn, 5, 0)` | 同上，优先级 5 = `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` |
| 13 | `App/app_adc.c` | `adc_init()` 里加 `HAL_ADCEx_Calibration_Start()` | STM32F1 的 ADC 上电必须校准，否则读数有固定偏差 |

> 第 10~12 项同时改回了 `MonitorTerminal.ioc`，**保持 `.ioc` 与代码一致**，
> 这样将来即使重新 GENERATE，这几项也不会退回错误值。

### 一句话总结这个改造

> **CubeMX 6.15 不给 STM32F1 开放 CMSIS-RTOS V2，所以我是"借 V1 的脚手架、换 V2 的封装层"。**
> 改造集中在 4 个文件：`.uvprojx`、`FreeRTOSConfig.h`、`freertos.c`、`main.c`。
>
> 换 V2 之后**多出四个必须手工补的东西**，这也是这套改造真正的技术内容：
> ①`osKernelInitialize()` ②V2 需要的十几个 config 开关
> ③defaultTask 的 `osThreadDef` 签名不兼容 ④静态分配回调由 `cmsis_os2.c` 自带、不能重复定义

---

## 遇到的问题与解决

> ⏳ **这一节留空，等真机跑起来后再写。**
>
> 预先编造排障过程是没有意义的 —— 这一节的价值恰恰在于它是**真实发生过的**。
> 计划中要重点记录的是：**两个任务并发访问同一条 I²C 总线时的冲突现象与定位过程**。
> 那正是本项目的核心，也是从"照着手册做"到"自己设计"的分界线。

---

## 已知局限

这些是**有意为之的取舍**，不是遗漏：

- **驱动层尚未在真机验证**。代码已通过编译检查，但硬件到货前无法确认运行时行为。
- **软件模拟 I²C 而不是硬件 I²C**。F103 的硬件 I²C 有出名的 BUSY 卡死勘误；
  本项目速率需求极低（1s 读一次），软件模拟更可控，且总线事务是一个连续的临界区，
  互斥锁边界更清晰。**代价是刷屏慢** —— 整屏约 150ms，所以显示周期定在 500ms。
- **SSD1306 刷新是全屏刷新，不是脏页刷新**。当前只刷新变化的页会更省总线时间，
  但实现复杂度上去了。在 500ms 周期下当前方案的总线占用率约 30%，可以接受。
- **字库只覆盖可打印 ASCII**，不显示中文。UTF-8 中文字符会被当成空格画出。
