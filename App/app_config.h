/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   全局配置：引脚、阈值、任务参数、协议常量，全部集中在这里。
  *
  * 为什么集中：调阈值不用翻遍所有 .c；面试时"你的参数在哪定义"一句话能答完。
  ******************************************************************************
  */

#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

#include "main.h"
#include <stdint.h>

/* ==========================================================================
 * 引脚
 *
 * 下面这些 _Pin / _GPIO_Port 宏由 CubeMX 依据 .ioc 里的 User Label 生成，
 * 定义在 main.h 中。所以**必须在 CubeMX 里把标签设成完全相同的名字**。
 *
 * 与其等到链接时报一堆看不懂的错，不如在这里直接卡住 —— 缺哪个就报哪个。
 * ========================================================================== */

/* ★★ 下面这些 #error 的文字**刻意只用 ASCII**。
 *
 *   踩过的坑：原来这里写的是中文，编译时报出来是乱码
 *   （`CubeMX 閰嶇疆锛歅B6 鐨?User Label ...`）——armcc 在中文 Windows 上
 *   处理 #error 里的非 ASCII 字符串不可靠，实测加 UTF-8 BOM 也没用，
 *   字节级比对确认第三个字节被替换成了 '?'。
 *
 *   **护栏的信息读不了，护栏就白设了。** 所以宁可写得朴素一点，
 *   也要保证它在任何编码环境下都能读。解释放在注释里，注释不影响编译。
 */

/** 软件 I²C 时钟线（PB6，开漏输出） */
#ifndef I2C_SCL_Pin
#error "CubeMX config missing: set PB6 User Label to I2C_SCL"
#endif
/** 软件 I²C 数据线（PB7，开漏输出） */
#ifndef I2C_SDA_Pin
#error "CubeMX config missing: set PB7 User Label to I2C_SDA"
#endif
/** 按键（PA3，EXTI 下降沿）—— 注意不是板载那个 PA0 */
#ifndef KEY_Pin
#error "CubeMX config missing: set PA3 User Label to KEY"
#endif
/** 蜂鸣器（PB0，推挽输出） */
#ifndef BEEP_Pin
#error "CubeMX config missing: set PB0 User Label to BEEP"
#endif
/** 板载 LED（PC13，低电平点亮）——CubeMX 建工程时已经叫 LED 了 */
#ifndef LED_Pin
#error "CubeMX config missing: set PC13 User Label to LED"
#endif

/* ==========================================================================
 * 软件 I²C
 *
 * 关于延时：I²C 标准模式上限 100kHz（半周期 5µs），快速模式 400kHz。
 * 本项目是 1s 读一次温湿度，速率毫无压力，取 100kHz 偏保守的值即可。
 * 72MHz 主频下 5µs ≈ 360 个时钟周期，用忙等实现。
 * ========================================================================== */
#define I2C_DELAY_US        5u      /**< 半周期延时（µs），对应约 100kHz */
#define I2C_TIMEOUT_MS      100u    /**< 单次 I²C 事务拿锁超时 */

/** 等待从机释放 SCL 的最大循环次数（防时钟拉伸死等）。
 *
 *  ★ 这个值不能调大。72MHz 下每次循环约 15 个周期，5000 次 ≈ 1ms。
 *    真实的时钟拉伸最多几百 µs，1ms 已经很宽裕。
 *
 *    为什么强调：如果模块板上**没有上拉电阻**（面包板上很常见），
 *    SCL 会永远读回 0，于是每个 bit 都把守卫跑满。一次 SHT30 事务约
 *    120 个 bit，就是 120ms；更要命的是这期间**互斥锁一直被持有**，
 *    TaskDisplay 全被堵住，现象是整个系统像卡死了。
 *    配合 app_i2c.c 里的"快速失败"（等不到就立刻返回 false），
 *    卡死会在第一个 bit 就暴露出来，而不是拖垮全系统。 */
#define I2C_STRETCH_LIMIT   5000u

/** 7 位地址左移 1 位后的 8 位形式（HAL 惯例，最低位是读写位） */
#define SHT30_I2C_ADDR      (0x44 << 1)
#define SSD1306_I2C_ADDR    (0x3C << 1)

/* ==========================================================================
 * 板载 LED
 *
 * ★ 蓝 pill 的 PC13 LED 是**低电平点亮**（阳极接 3.3V，阴极接 PC13）。
 *   所以"点亮"要写 RESET，"熄灭"要写 SET。搞反了 LED 就常亮或常灭。
 * ========================================================================== */
#define LED_ON()    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET)
#define LED_OFF()   HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET)
#define LED_TOGGLE() HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin)

/** 蜂鸣器，高电平响 */
#define BEEP_ON()   HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET)
#define BEEP_OFF()  HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET)

/* ==========================================================================
 * ADC + DMA（双通道循环搬运）
 *
 * PA1 = 光敏电阻分压，PA2 = 电位器。两路都接 3.3V，绝不能接 5V
 * ——ADC 输入超过 VDDA 会打坏引脚。
 *
 * ★ 光敏用 10kΩ 上拉分压：亮阻 10~20kΩ（10lux）、暗阻 1MΩ，
 *   配 10kΩ 时典型光照下分压点落在量程中点附近，动态范围最大。
 *   配 1kΩ 只能用掉不到 10% 量程。
 * ========================================================================== */
#define ADC_DMA_LEN         2u      /**< 通道数，也就是 DMA 搬运的数组长度 */
#define ADC_CH_LIGHT        0u      /**< adcDmaBuf[0] ← PA1 */
#define ADC_CH_POT          1u      /**< adcDmaBuf[1] ← PA2 */
#define ADC_FILTER_WIN      8u      /**< 滑动平均窗口点数 */
#define ADC_FULL_SCALE      4095u   /**< 12 位 ADC 满量程 */

/* ==========================================================================
 * SSD1306 OLED
 *
 * 128×64 单色屏，按页组织：每页 8 行像素，8 页共 64 行。
 * 显存格式与屏的原生格式一致（一字节 = 竖向 8 个像素，bit0 最上），
 * 所以刷新时整块 framebuffer 可以原样搬过去，不需要逐位转换。
 * ========================================================================== */
#define SSD1306_WIDTH        128u
#define SSD1306_HEIGHT       64u
#define SSD1306_PAGE_COUNT   (SSD1306_HEIGHT / 8u)   /**< 8 页 */

/** ★ 每次 I²C 事务写入的页数 —— 直接决定**单次持锁时长**。
 *
 *  为什么必须分片：软件模拟 I²C 实测约 60kHz，一个字节约 9 bit × 16µs
 *  ≈ 144µs。整屏 1024 字节要 **约 150ms**。如果一次事务刷完整屏，
 *  这 150ms 内互斥锁一直被 TaskDisplay 占着，TaskSensor 读温湿度
 *  只能干等 —— 互斥锁本来是为了消除冲突，这样反而制造了长时间阻塞。
 *
 *  取 2 页：单次事务 256 字节 ≈ 区块命令 + 256 数据 ≈ 38ms 持锁。
 *  分片之间释放锁，传感器最多只等一个分片。
 *
 *  ⚠️ 这个数字是**按 60kHz 估算**的，真机上用逻辑分析仪或示波器
 *     量一下 SCL 实际频率后应该复核一遍。
 */
#define SSD1306_FLUSH_CHUNK_PAGES   2u

/* ==========================================================================
 * 报警阈值
 *
 * 温湿度沿用实训里验证过的那组区间，方便和实训经历对上。
 * ========================================================================== */
#define TH_TEMP_LOW_X100    2000    /**< 20.00 ℃ */
#define TH_TEMP_HIGH_X100   3000    /**< 30.00 ℃ */
#define TH_HUMI_LOW_X100    3000    /**< 30.00 %RH */
#define TH_HUMI_HIGH_X100   8000    /**< 80.00 %RH */
#define TH_LIGHT_LOW        200u    /**< 200 lx 对应 ADC 原始值，需实测标定 */
#define TH_LIGHT_HIGH       550u    /**< 550 lx 对应 ADC 原始值，需实测标定 */

/* ==========================================================================
 * FreeRTOS 任务参数
 *
 * ★★ 注意 osThreadAttr_t.stack_size 的单位是**字节**，不是字！
 *    CMSIS-RTOS2 的经典坑：写 128 以为给了 128 个字，实际只给了 32 个字，
 *    一跑就 HardFault。所以这里统一用 _STACK_BYTES 后缀，乘 4 换算。
 *
 * 优先级**不在这里定义** —— CMSIS-RTOS2 用的是 osPriority_t 枚举而不是
 * 数字，且映射到 FreeRTOS 后只有 5 个可用档位。定义放在 app_tasks.c 里，
 * 紧挨着任务创建代码，避免两处口径不一致。
 * ========================================================================== */

/** 采集任务：50ms 硬周期，最高的实时性要求 */
#define TASK_ACQUIRE_STACK_BYTES (128u * 4u)   /**< 128 字 */
#define TASK_ACQUIRE_PERIOD_MS  50u

/** 传感器任务：读 SHT30 一次约 15ms（软件 I²C 阻塞），周期 1s */
#define TASK_SENSOR_STACK_BYTES (256u * 4u)    /**< 256 字 */
#define TASK_SENSOR_PERIOD_MS   1000u

/** 处理任务：阻塞在事件组上，数据齐了才跑 */
#define TASK_PROCESS_STACK_BYTES (192u * 4u)   /**< 192 字 */

/** 显示任务：★ 周期从方案原定的 200ms 改成 500ms。
 *
 *  原因：软件模拟 I²C 约 60kHz，刷满整屏 1024 字节约需 **150ms**
 *  （方案里"刷一屏十几毫秒"是**硬件 I²C** 的数字，不适用于本项目）。
 *  200ms 周期意味着总线 75% 的时间被占，传感器会频繁排队。
 *  500ms 周期下占比降到 30%，人眼也完全看不出闪烁。 */
#define TASK_DISPLAY_STACK_BYTES (256u * 4u)   /**< 256 字 */
#define TASK_DISPLAY_PERIOD_MS  500u

/** 通信任务：阻塞等事件组，打包后走 UART DMA */
#define TASK_COMM_STACK_BYTES   (256u * 4u)    /**< 256 字 */

/** 监控任务：喂狗 + 查心跳 + 驱动蜂鸣器 */
#define TASK_MONITOR_STACK_BYTES (128u * 4u)   /**< 128 字 */
#define TASK_MONITOR_PERIOD_MS  500u

/* ==========================================================================
 * 队列 / 事件组
 * ========================================================================== */

/** 采集原始值的队列深度。发得比收得快时靠它吸收抖动 */
#define QUEUE_SAMPLE_DEPTH      8u
/** SHT30 读数队列深度 */
#define QUEUE_SENSOR_DEPTH      4u
/** 显示任务的更新队列深度 */
#define QUEUE_DISPLAY_DEPTH     4u

/** 事件组的位定义。
 *
 *  前两位是"数据源就绪"，TaskProcess 用 osFlagsWaitAll 等这两位**同时**置位
 *  —— 这就是事件组的典型用途：**多源同步**，等所有条件齐了再往下走。
 *
 *  报警位则是**状态广播**：TaskProcess 负责置/清，TaskMonitor 用
 *  osEventFlagsGet 直接读，不需要再开一条队列传这个单 bit 的信息。 */
#define EVT_FLAG_ADC_READY      (1UL << 0)
#define EVT_FLAG_SHT30_READY    (1UL << 1)
#define EVT_FLAG_ALARM          (1UL << 2)
/** TaskProcess 等的就是这两位都到齐 */
#define EVT_FLAG_DATA_READY     (EVT_FLAG_ADC_READY | EVT_FLAG_SHT30_READY)

/* ==========================================================================
 * 看门狗与心跳
 *
 * IWDG 时钟源是 LSI（F103 上典型值 40kHz，**不准且随温度漂移**）。
 * 超时 = (4 * 2^prescaler / LSI) * reload
 *      = (4 * 2^3 / 40000) * 1250 = 1.0s
 * 取 1s 是留足余量：最慢的任务周期是 1s，给它两次机会。
 * ========================================================================== */
#define IWDG_PRESCALER          IWDG_PRESCALER_8
#define IWDG_RELOAD             1250u   /**< 约 1.0s 超时（按 LSI=40kHz 估算） */
#define HEARTBEAT_STALL_LIMIT   3u      /**< 连续 3 次没涨心跳就判定任务卡死 */

/* ==========================================================================
 * 串口协议（与 Host/frame_codec.py 严格对称）
 *
 *   偏移  字段     长度  说明
 *    0    0xAA     1     帧头 1
 *    1    0x55     1     帧头 2
 *    2    LEN      1     数据区长度
 *    3    SEQ      1     帧序号 0~255 循环
 *    4-5  温度     2     int16 大端，×100
 *    6-7  湿度     2     int16 大端，×100
 *    8-9  光照     2     uint16 大端，ADC 原始值
 *   10-11 电位器   2     uint16 大端，ADC 原始值
 *   12-13 CRC16    2     Modbus CRC16，低字节在前
 *
 * 数据区大端、CRC 低字节在前 —— 这是 Modbus-RTU 的惯例：
 * Modbus 串行链路上 CRC 就是低字节先发。不是随手定的。
 * ========================================================================== */
#define FRAME_HEADER0           0xAAu
#define FRAME_HEADER1           0x55u
#define FRAME_DATA_LEN          8u      /**< 当前固件发送的数据区长度 */
#define FRAME_META_LEN          2u      /**< LEN + SEQ */
#define FRAME_CRC_LEN           2u
#define FRAME_TOTAL_LEN         (2u + FRAME_META_LEN + FRAME_DATA_LEN + FRAME_CRC_LEN)

/** 通道缩放：温度湿度 ×100 发整数，避免浮点传输和字节序争议 */
#define CH_SCALE                100

/** UART1 —— 接 CH340，蓝 pill 那个 USB 口不能当串口用 */
#define UART_BAUDRATE           115200u
#define UART_TX_TIMEOUT_MS      100u
/** 单条日志的最大长度（字节）。超出部分被截断，不会溢出缓冲区 */
#define UART_LOG_BUF_SIZE       128u

#endif /* __APP_CONFIG_H */
