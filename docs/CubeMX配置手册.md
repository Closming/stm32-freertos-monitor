# CubeMX 配置手册 —— 从"只能点灯"到"跑得动这套代码"

> **目标**：在现有裸机点灯工程上追加外设与 FreeRTOS，让 `App/` 目录的代码能编译、能跑。
> **完成标志**：`bash Tools/check_app.sh` 全过 **且** Keil 编译 0 Error，烧录后 OLED 亮起、串口有帧输出。
>
> 在**现有 `.ioc` 上改**，不要新建工程 —— 新建会丢掉已经调好的时钟树和器件配置。

---

## 第 0 步 · 打开工程

打开 STM32CubeMX（`D:\STM32CubeMx\STM32CubeMX.exe`）→ `File` → `Load Project`
→ 选 `D:\stm32\MonitorTerminal\MonitorTerminal.ioc`。

---

## 第 1 步 · USART1（接 CH340）

左侧 `Connectivity` → **USART1**

| 选项 | 设为 |
|---|---|
| Mode | **Asynchronous** |
| Hardware Flow Control | Disable |
| Baud Rate | **115200** |
| Word Length | 8 Bits |
| Parity | None |
| Stop Bits | 1 |

配好后 **PA9 / PA10** 会自动变成绿色（`USART1_TX` / `USART1_RX`）。

**再加 DMA：** 切到 **DMA Settings** 标签页 → `Add` → 选 **USART1_TX**

| 参数 | 设为 |
|---|---|
| Direction | Memory To Peripheral |
| Priority | Low |
| **Mode** | **Normal**（★ 不是 Circular） |
| Data Width (Peripheral) | **Byte** |
| Data Width (Memory) | **Byte** |
| Increment Address | Memory ☑ / Peripheral ☐ |

> ⚠️ **Mode 必须是 Normal。** 用 Circular 的话 DMA 会无限循环重发同一块缓冲区，
> 串口上会看到同一帧被反复发出来 —— 现象很迷惑，第一眼会以为程序卡在循环里。

---

## 第 2 步 · ADC1 + DMA（光敏 + 电位器）

左侧 `Analog` → **ADC1**

### ① 先在 Mode 面板里勾通道

> ⚠️ **顺序很关键：`Parameter Settings` 标签页在你勾通道之前是不会出现的。**
> 第一次配 ADC 的人常在这里找半天"参数设置在哪"。

在 ADC1 面板的 **Mode** 区（上半部分）勾选：

```
☐ IN0                  ← PA0，★ 不要勾（板载按键在那，误按会让读数饱和）
☑ IN1                  ← PA1，光敏电阻
☑ IN2                  ← PA2，电位器
☐ Temperature Sensor   ← ★ 不要勾，见下方说明
☐ Vrefint Channel      ← ★ 不要勾，见下方说明
```

> **★ 两个内部通道（Temperature Sensor / Vrefint）绝对不能勾。**
>
> 它们不是外部引脚，而是芯片内部的通道（ADC1_IN16 / IN17）：前者测的是
> **MCU 自己的结温**（不是环境温度），后者是内部基准电压。本项目都用不上。
>
> **真正的风险是 DMA 写越界：** `app_adc.c` 里是
> `static uint16_t s_dma_buf[ADC_DMA_LEN]`，而 `ADC_DMA_LEN = 2`。
> DMA 的搬运长度由 **CubeMX 配在 DMA 控制器里**，和代码里传的那个参数
> 是**两套独立的配置** —— 启用了 4 个通道，DMA 就会往 2 元素的数组里写 4 个半字，
> **后两个踩到数组后面的内存**。
>
> 现象是"某个毫不相干的变量莫名变值"或随机 HardFault，**不报任何编译错误**。
>
> 将来若真要加内部通道，必须 **CubeMX 的通道数和 `ADC_DMA_LEN` 同时改**。

勾完应该立刻看到**芯片图上 PA1 / PA2 变绿**，鼠标悬停显示 `ADC1_IN1` / `ADC1_IN2`。

> 通道是按 `IN0~IN9` 编号的，**不是按引脚名**。STM32F103C8 的对应关系：
> IN0~IN7 → PA0~PA7，IN8/IN9 → PB0/PB1。

### ② Parameter Settings 标签页

（勾完上面两个通道，这个标签页才出现）

**Parameter Settings 里分成两个区块**，别只看到上面那个就以为没有了：

```
ADC_Settings                        ← 上半部分
├─ Data Alignment
├─ Scan Conversion Mode             ← 灰的，由下面区块的值决定
├─ Continuous Conversion Mode
└─ Discontinuous Conversion Mode

Regular Conversion Settings         ← ★ 往下翻！Number Of Conversion 在这个区块
├─ Enable Regular Conversions
├─ Number Of Conversion             ← ★★ 必须是 2
├─ External Trigger Conversion Source
└─ Rank 表
```

| 选项 | 所在区块 | 设为 | 为什么 |
|---|---|---|---|
| **Enable Regular Conversions** | Regular Conversion Settings | **Enable** | 设成 Disable 会把 Number Of Conversion 锁死为 1，下面全部失效 |
| ★★ **Number Of Conversion** | Regular Conversion Settings | **2** | ★★ **这是扫描模式的总开关**，见下方说明 |
| **Scan Conversion Mode** | ADC_Settings | **Enabled**（灰，自动） | 只有当 Number Of Conversion > 1 时才可能为 Enabled |
| ★★ **Continuous Conversion Mode** | ADC_Settings | **Enabled** | ★★ 见下方说明，**这个设错了不会报错** |
| Discontinuous Conversion Mode | ADC_Settings | Disabled | |
| Data Alignment | ADC_Settings | Right alignment | |
| External Trigger Conversion Source | Regular Conversion Settings | **Regular Conversion launched by software** | 软件触发即可 |

> ### ★★ `Scan Conversion Mode` 为什么是灰的 Disabled
>
> 这个选项**不是自由选择的**，它被 `Number Of Conversion` 门控。
> 这一点读 CubeMX 自己的定义文件可以确认
> （`db/mcu/IP/ADC-aditf_v2_5F1_Cube_Modes.xml`）：
>
> ```xml
> <PossibleValue Comment="Disabled" Value="ADC_SCAN_DISABLE"
>     Condition="(( NbrOfConversion > 1 &amp; EnableRegularConversion = ENABLE) | ...)"
>     Diagnostic="Scan conversion mode disabled only when both Regular
>                 and Injected conversion Rank &lt;= 1"/>
> ```
>
> 翻译过来：
>
> ```
> Number Of Conversion = 1  →  扫描模式锁死 Disabled（灰）← 一路没啥好扫的
> Number Of Conversion ≥ 2  →  扫描模式自动变 Enabled（灰）
> ```
>
> **所以看到「灰 + Disabled」，不要去点它，回去把 `Number Of Conversion` 改成 2。**
> 改完它会自己变成 Enabled，Rank 表也会多出一行。

> ### ★★ 为什么 `Continuous Conversion Mode` 必须是 `Enabled`
>
> 这两件事很容易混为一谈，但它们**互不替代**：
>
> | | 是什么 | 管什么 |
> |---|---|---|
> | **ADC 连续转换模式** | ADC 自己一轮接一轮地转 | **产生数据** |
> | **DMA 循环模式**（DMA Settings 里那个 Circular） | DMA 写满缓冲区后绕回开头 | **搬运数据** |
>
> **只开 DMA 循环、把连续转换关掉的话：**
>
> ```
> ADC 转一轮（IN1 → IN2）→ 停
> DMA 把这两个值搬进缓冲区 → 绕回开头等着
> ADC 再也不产生新数据 → 缓冲区里永远是同一组值
> ```
>
> **现象：串口一直在发帧、OLED 一直在刷新，但读数永远是开机那一刻的值，
> 一动不动 —— 而且不报任何错误。**
>
> 如果只想让 ADC 转一次，那才用 Disabled（配合外部触发）。
> 本项目要的是**持续监测**，所以必须 Enabled。

> ### ★ Mode 面板里的 `Conversion Trigger` 是什么
>
> **它不是软件/定时器触发的选择**，而是「**要不要用 EXTI 外部引脚触发**」：
>
> ```xml
> <Mode UserName="Conversion Trigger">
>     <Mode UserName="Injected Trigger">             ← EXTI15 引脚
>     <Mode UserName="Regular Trigger">              ← EXTI11 引脚
>     <Mode UserName="Injected and Regular Trigger"> ← 两个都用
> ```
>
> 本项目用软件触发，所以**保持 `Disable` 就对了**，不用管它。

> ### ★★ 为什么 `Continuous Conversion Mode` 必须是 `Enabled`
>
> 这两件事很容易混为一谈，但它们**互不替代**：
>
> | | 是什么 | 管什么 |
> |---|---|---|
> | **ADC 连续转换模式** | ADC 自己一轮接一轮地转 | **产生数据** |
> | **DMA 循环模式**（DMA Settings 里那个 Circular） | DMA 写满缓冲区后绕回开头 | **搬运数据** |
>
> **只开 DMA 循环、把连续转换关掉的话：**
>
> ```
> ADC 转一轮（IN1 → IN2）→ 停
> DMA 把这两个值搬进缓冲区 → 绕回开头等着
> ADC 再也不产生新数据 → 缓冲区里永远是同一组值
> ```
>
> **现象：串口一直在发帧、OLED 一直在刷新，但读数永远是开机那一刻的值，
> 一动不动 —— 而且不报任何错误。**
>
> 如果只想让 ADC 转一次，那才用 Disabled（配合外部触发）。
> 本项目要的是**持续监测**，所以必须 Enabled。

下面 Rank 表两行 —— **Channel 那一格是下拉框，要手动点开选**：

| | Channel | Sampling Time |
|---|---|---|
| Rank 1 | **Channel 1**（= ADC_IN1 = PA1，光敏） | **55.5 Cycles** |
| Rank 2 | **Channel 2**（= ADC_IN2 = PA2，电位器） | **55.5 Cycles** |

> 📌 **两套叫法指的是同一个东西**，看你界面上显示的是哪种：
> `Channel 1/2` 和 `ADC_IN1/IN2` 是同一个通道。
> 下拉框里通常**只列出你在 Mode 面板勾选的那几个**，所以选项少是正常的。
>
> **★ 选完必须验证**：中间的芯片图上 **PA1 / PA2 要变绿**，悬停显示 `ADC1_IN1` / `ADC1_IN2`。
> 没变绿就是没选上；变绿的是别的引脚就是选错了。

**DMA Settings 标签页：** `Add` → **ADC1**

| 参数 | 设为 |
|---|---|
| Direction | Peripheral To Memory |
| Priority | High |
| **Mode** | **Circular** | ★ 这里必须是 Circular，让 DMA 一直循环搬运 |
| Data Width (Peripheral) | **Half Word** | ★ ADC 是 12 位，用半字 |
| Data Width (Memory) | **Half Word** | ★ 必须是 Half Word，代码里是 `uint16_t` 数组 |
| Increment Address | Memory ☑ / Peripheral ☐ |

> ⚠️ **Data Width 必须是 Half Word。** 设成 Word 的话每个采样占 4 字节，
> 代码里按 `uint16_t[2]` 读会读到错位的数据 —— 而且**不报错**，只是数值全乱。

**时钟检查**：切到 `Clock Configuration` 标签页，确认 **ADC Prescaler = /6**
（APB2 = 72MHz，÷6 = 12MHz ≤ ADC 上限 14MHz）。CubeMX 一般会自动设对，
但如果这里红了或者警告，就是这个问题。

---

## 第 3 步 · 软件 I²C 的两根线（PB6 / PB7）

在中间的芯片图上**分别左键点 PB6、PB7** → 各选 **GPIO_Output**。

然后左侧 `System Core` → **GPIO**，在下方列表里逐行双击修改：

**PB6 那一行：**

| 参数 | 值 |
|---|---|
| GPIO output level | **High** |
| GPIO mode | **Output Open Drain** | ★★ 必须是开漏，不能是推挽 |
| GPIO Pull-up/Pull-down | **Pull-up** | ★ 见下 |
| Maximum output speed | **High** |
| User Label | **`I2C_SCL`** | ★★ 名字必须一字不差 |

**PB7 那一行：** 同上，User Label 填 **`I2C_SDA`**。

> ⚠️ **必须是 Open Drain。** I²C 是"线与"总线，多个器件都能把线拉低，
> 但**谁都不能主动驱高**。推挽输出在两边电平不一致时会产生大电流，
> 轻则通信失败，重则打坏引脚。
>
> ⚠️ **Pull-up 建议开。** SHT30 / OLED 模块通常板载了 4.7kΩ 上拉，开了内部的
> 只是并联、无害；但**万一模块没有板载上拉**，不开的话总线完全不通
> （现象：读不到任何 ACK，`app_i2c.c` 里的快速失败会让每次事务瞬间返回 false）。
> 内部上拉约 40kΩ 偏弱，低速下够用，可以先跑起来验证。

**★ User Label 必须精确匹配。** `app_config.h` 里是这么写的：

```c
#ifndef I2C_SCL_Pin
#error "CubeMX 配置：PB6 的 User Label 必须设为 I2C_SCL"
#endif
```

名字填错的话，编译时**第一句报错就会直接告诉你**缺哪个标签，
不会让你在一屏"标识符未定义"里去猜。

---

## 第 4 步 · 按键（PA3）

芯片图上点 **PA3** → 选 **GPIO_EXTI3**。

`System Core` → **GPIO** 里改 PA3 那一行：

| 参数 | 值 |
|---|---|
| GPIO mode | **External Interrupt Mode with Falling edge trigger detection** |
| GPIO Pull-up/Pull-down | **Pull-up** | ★ 按键另一端接 GND，必须上拉 |
| User Label | **`KEY`** |

> **★ 为什么是 PA3 而不是板载按键所在的 PA0：** PA0 留给了 ADC，见下方引脚表。
> 板载按键误按会往 PA0 灌 3.3V，让光敏读数瞬间饱和。

**然后必须配 NVIC** —— 切到 `NVIC Settings` 标签页，
勾选 ☑ **EXTI line3 interrupt**，并把 **Preemption Priority 设为 5 或更大**。

> ⚠️⚠️ **优先级这一条搞错会随机死机。**
>
> `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 默认是 **5**。
> 优先级**数值小于 5** 的中断，**不允许调用任何 FreeRTOS 的 `...FromISR` API**。
> 而我们的按键中断里要调 `osThreadFlagsSet()`（它内部走 `xTaskNotifyFromISR`）。
>
> 如果给了 0~4，FreeRTOS 会直接触发 `configASSERT` 卡死，或者更糟 ——
> 在内核数据结构上产生竞态，表现为**偶尔**跑飞。这类问题极难定位。
>
> **记法：数值越大优先级越低。要调 FreeRTOS API，数值必须 ≥ 5。**

---

## 第 5 步 · 蜂鸣器（PB0）

芯片图上点 **PB0** → **GPIO_Output**。

`System Core` → **GPIO** 里改：

| 参数 | 值 |
|---|---|
| GPIO output level | **Low** | ★ 上电不响 |
| GPIO mode | **Output Push Pull** |
| GPIO Pull-up/Pull-down | No pull-up and no pull-down |
| Maximum output speed | Low |
| User Label | **`BEEP`** |

---

## 第 6 步 · 独立看门狗 IWDG

左侧 `System Core` → **IWDG**

| 选项 | 设为 |
|---|---|
| Activated | ☑ 勾上 |
| IWDG counter clock prescaler | **8** |
| IWDG down-counter reload value | **1250** |

超时按 LSI ≈ 40kHz 估算：

```
超时 = (4 × 2^prescaler / LSI) × reload
     = (4 × 8 / 40000) × 1250 = 1.0 s
```

> ⚠️ **LSI 不准**（典型值 40kHz，实际 30~50kHz，且随温度和电压漂移），
> 所以真实超时可能是 0.8~1.3s。取 1s 是留了余量：最慢的任务周期是 1s，
> 让它至少能喂两次狗。
>
> ⚠️ IWDG 一旦启动就**停不下来**，也不能改配置，只有复位才能重来。
> 所以调试时如果程序卡在断点上超过 1s，板子会被看门狗复位 —— 这是正常的，
> 不是 bug。真要在断点上停留，先在 CubeMX 里把 IWDG 关掉。

---

## 第 7 步 · FreeRTOS

左侧 `Middleware and Software Packs` → **FREERTOS**

**Interface 选 ★ `CMSIS_V1`。**

> ### ★★ 为什么是 V1 而不是 V2（很重要，别选错也別奇怪）
>
> **CubeMX 6.15 不提供 STM32F1 的 CMSIS_V2 选项** —— V2 那个选项是**灰色不可选**的。
> 这不是配置错误，是 CubeMX 数据库的硬限制。读它自己的定义文件可以确认
> （`db/mcu/IP/FREERTOS-v8.0.0_Cube_Modes.xml`）：
>
> ```xml
> <Mode Name="CMSIS_V2">
>     <Condition Expression="STM32F4|STM32F7|STM32G0|STM32G4|STM32H7|STM32L1|STM32L4|STM32MP1|STM32WB"
>                Diagnostic="Available for F4,F7,G0,G4,H7,L1,L4,MP1 and WB series"/>
> </Mode>
> ```
>
> **`STM32F1` 不在那个列表里。** 旁边还有句注释：`<!-- "CMSIS_V2" being delivered series after series -->`
> —— 逐系列放开的，F1 还没轮到。
>
> **但 V2 的源码是齐的**，就在固件包里：
> `STM32Cube_FW_F1_V1.8.4\Middlewares\Third_Party\FreeRTOS\Source\CMSIS_RTOS_V2\`
>
> 所以做法是：**让它按 CMSIS_V1 生成脚手架，再把封装层换成 V2。**
> 详见 `docs/CMSIS_V1转V2操作手册.md`（生成后编写）。

**然后进 `Tasks and Queues` 标签页 → 选中 `defaultTask` → 点 `Delete`。**

> ★ **必须删。** 六个任务全部由 `app_init()` 创建，留着 defaultTask 会白占
> 128 字栈和一个任务槽，而且它的空循环毫无意义。

**`Config parameters` 标签页** —— 确认这两项（默认值通常就对）：

| 选项 | 值 | 为什么 |
|---|---|---|
| `configMAX_PRIORITIES` | **7** | 六任务需要 5 个不同档位，7 够用 |
| `configUSE_MUTEXES` | **Enabled** | ★★ 不开的话优先级继承**完全失效**，互斥锁退化成普通二值信号量 |

> ⚠️ **`configUSE_MUTEXES` 是这个项目的命门。** 我们的核心设计是
> "互斥锁 + 优先级继承"解决 I²C 总线争用。不开这个选项，锁还能用，
> 但**优先级继承不生效**，低优先级任务持锁时高优先级任务仍会被中优先级
> 任务饿死 —— 现象是"加了锁还是偶尔卡"，比不加锁更难查。

---

## 第 8 步 · 检查 NVIC 汇总

`System Core` → **NVIC**，确认这几项已使能且**优先级 ≥ 5**：

| 中断 | 优先级 | 用途 |
|---|---|---|
| `EXTI line3 interrupt` | **5** | 按键 |
| `DMA1 channel4 global interrupt` | **5** | ★ USART1_TX 的 DMA 完成中断 |
| `DMA1 channel1 global interrupt` | **5** | ADC 的 DMA |

> ⚠️ **DMA1 Channel4 一定要使能。** 它是 USART1_TX 的 DMA 通道。
> 不使能的话 `HAL_UART_Transmit_DMA()` 会成功启动，但
> **永远不会调用 `HAL_UART_TxCpltCallback`** —— 于是 `uart_send_frame()`
> 里等信号量会一直等到超时。现象是"串口能发出数据，但每帧都卡 100ms"。
>
> 它的优先级同样必须 ≥ 5，因为回调里要调 `osSemaphoreRelease`。

> 📌 验证方法：CubeMX 里如果优先级设成 0~4，多数版本会直接标红提示
> "Priority is not compatible with FreeRTOS"。看到这个提示就是它在保护你。

---

## 第 9 步 · GENERATE CODE 之前先确认

`Project Manager` → **Code Generator** 标签页：

- ☑ **Copy only the necessary library files**
- ☑ **Generate peripheral initialization as a pair of .c/.h files per peripheral**
- ☑ ★★ **Keep User Code when re-generating**

> ★ 最后一条千万别取消。`main.c` 里那段点灯代码就在 `USER CODE` 区块内，
> 取消勾选的话会被清掉。

---

## 第 10 步 · GENERATE CODE

点右上角 **GENERATE CODE**。

### ★★ 生成后立刻做这件事

**打开 Keil（或如果 Keil 已经开着，先关掉再开）→ `Alt + F7` → 看 `Device` 页。**

**如果器件列表是空的、显示 `See Pack Installer GUI for details`：**

这就是已知的那个坑 —— CubeMX 重写 `.uvprojx` 时把 `<Device/>` 清空了。
不修的话编译会报：

```
Error #550: Requested device ??device??(STMicroelectronics) not found for target 'MonitorTerminal'
```

**修法（只需做一次）：**

1. `Alt + F7` → **Device** 页
2. Vendor 选 `STMicroelectronics`
3. 器件选 **`STM32F103C8`**
4. OK —— Keil 会自动把 `<Device>` / `<FlashDriverDll>` / `<SFDFile>` 一起补进 `.uvprojx`

> ⚠️ **别用"命令行编译成功"来判断工程配置没问题。**
> 实测过：批处理构建能吃 `<Cpu>` 里硬编的 IRAM/IROM 直接编过，
> `<Device/>` 空着也报 `0 Error`。**只有 GUI 里的 Device 页能说明问题。**

---

## 第 11 步 · 把 App 目录加进 Keil 工程

Keil 里：

1. 左侧 Project 树 → 右键 Target → **Manage Project Items**
2. **Groups** 栏 → `New (Insert)` → 命名 **`App`**
3. 选中 App → **Files** 栏 → `Add Files` → 定位到 `..\App\`
4. 全选这 8 个 `.c` 文件 → Add → Close

```
app_adc.c        app_i2c.c        app_protocol.c   app_sht30.c
app_ssd1306.c    app_tasks.c      app_uart.c       app_font.c
```

> 📌 `.h` 文件不用手动加，编译器靠 IncludePath 找。

5. `Alt + F7` → **C/C++** 标签页 → `Include Paths` 右边点 `...`
6. **新增一行 `..\App`**

> ⚠️ 注意 CubeMX 生成的其他路径用的是 `../Inc` 这种**正斜杠**。两种 Keil 都认，
> 但保持一致比较清爽。

---

## 第 12 步 · 把 app_init() 接到 FreeRTOS 启动流程里

生成后 Keil 工程里会多出 `Src/freertos.c`。打开它，找到
`MX_FREERTOS_Init()` 函数，在 **USER CODE 区块内**加上一行：

```c
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* ☟☟ 加这一行 ☟☟ */
  app_init();          /* 创建队列/事件组/互斥锁，并创建六个任务 */

  /* USER CODE BEGIN RTOS_THREADS */
  /* USER CODE END RTOS_THREADS */
}
```

并在文件顶部（`USER CODE BEGIN Includes` 区块内）加上：

```c
#include "app_tasks.h"
```

> 📌 `MX_FREERTOS_Init()` 是在 `main()` 里、`osKernelStart()` **之前**调用的，
> 所以 `app_init()` 里能创建任务和队列，但**不能获取锁** ——
> 这也是 OLED 初始化被放在 `TaskDisplay` 第一轮循环里的原因。

---

## 第 13 步 · 编译验证

```bash
cd "D:/stm32/MonitorTerminal/MDK-ARM"
"G:/keil/UV4/UV4.exe" -b "MonitorTerminal.uvprojx" -j0 -o build.log
echo "退出码: $?"      # 0 = 无错
```

**预期**：`0 Error`。代码体积会从裸机的 `Code≈2.5KB` 明显涨上去
（FreeRTOS 内核 + 字库 + 各驱动），大概 15~20KB —— 64KB flash 绰绰有余。

> 📌 编译前先跑一遍 `bash Tools/check_app.sh`。它能查出 App 目录自己的语法/
> 语义问题，把"是自己写错了"和"是 CubeMX 没配好"这两类问题分开，
> 定位快得多。

---

## 常见故障速查

| 现象 | 原因 |
|---|---|
| 编译报 `#error "CubeMX 配置：XX 的 User Label 必须设为 YY"` | 引脚 User Label 拼错了，按提示改 CubeMX |
| 编译报 `Error #550: device not found` | `<Device/>` 被清空，见第 10 步 |
| 编译报 `stm32f1xx_hal_adc.h: No such file` | CubeMX 里 ADC 没使能，或没重新 GENERATE |
| ADC1 里**找不到 `Parameter Settings` 标签页** | 通道要先在 **Mode 面板**里勾（IN1 / IN2），勾完标签页才出现 |
| ADC 通道下拉框只有 `IN0`/`IN1` 两个选项 | 不正常。正常应能看到 IN0 ~ IN9（F103C8 的全部外部通道） |
| ★ **找不到 `Number Of Conversion`** | 它在 **`Regular Conversion Settings`** 区块里，在 `ADC_Settings` **下面**，需要往下翻 |
| ★ `Scan Conversion Mode` **灰着显示 Disabled** | `Number Of Conversion` 是 1。它被这个值**门控**，改成 2 后会自动变 Enabled |
| ★ Mode 面板的 `Conversion Trigger` 显示 Disable | **正常**。它问的是"要不要用 EXTI 外部引脚触发"，我们用软件触发，保持 Disable |
| 运行后 `osMutexNew` 返回 NULL，进 Error_Handler | FreeRTOS 里 `configUSE_MUTEXES` 没开 |
| 按键一按就卡死 / 随机跑飞 | EXTI3 的 NVIC 优先级 < 5，中断里调了 FreeRTOS API |
| 串口一帧都收不到 | ① CH340 接线（TX/RX 交叉、共地）；② 波特率；③ DMA1_Channel4 中断没使能 |
| 串口每帧卡 100ms 才发下一帧 | DMA1 Channel4 中断没使能，`HAL_UART_TxCpltCallback` 从不触发 |
| 上位机一帧都解析不出来，但串口有数据 | 波特率不对，或 C 侧 CRC 表被改动过（跑 `test_codec.py` 能查出来） |
| OLED 完全不亮 | ① 电荷泵命令没发（代码里有，检查驱动）；② I²C 没上拉；③ 地址不是 0x3C |
| OLED 亮但显示乱码 | 器件地址搞混了（SHT30 是 0x44、OLED 是 0x3C），或 I²C 时序太快 |
| 读数偶发错乱 | **★ 这就是本项目的核心问题，别急着改。先录屏、记复现条件** |
| 板子每隔 1s 复位一次 | 看门狗在喂，但有任务卡死了。看串口有没有 `[WDT] task XXX STALLED` |
| ★ 串口一直在发帧，但**读数值恒定不变** | ADC 的 `Continuous Conversion Mode` 没开。**DMA 循环 ≠ ADC 连续转换**，两个都要开 |
| ADC 读到的两路值一样 / 明显错位 | DMA 的 Data Width 不是 Half Word，或通道顺序和 `ADC_CH_xxx` 对不上 |

---

## 完成清单

- [ ] USART1 配好（115200，DMA TX，**Normal** 模式）
- [ ] ADC1 双通道 + DMA（**Circular**，**Half Word**）
- [ ] PB6/PB7 = **Open Drain** + Pull-up，Label `I2C_SCL` / `I2C_SDA`
- [ ] PA3 = EXTI3 下降沿 + Pull-up，Label `KEY`，**NVIC 优先级 ≥ 5**
- [ ] PB0 = 推挽输出，Label `BEEP`
- [ ] IWDG 使能（prescaler 8 / reload 1250 ≈ 1s）
- [ ] FREERTOS = **CMSIS_V2**，**defaultTask 已删**，`configUSE_MUTEXES` 已开
- [ ] `DMA1_Channel4` / `DMA1_Channel1` 中断已使能，优先级 ≥ 5
- [ ] Code Generator 勾了 **Keep User Code**
- [ ] GENERATE CODE 后 **`Alt+F7` 检查过 Device 页**
- [ ] `App` 分组已加进 Keil，IncludePath 含 `..\App`
- [ ] `freertos.c` 里调用了 `app_init()`
- [ ] `bash Tools/check_app.sh` 全过 **且** Keil 编译 0 Error
