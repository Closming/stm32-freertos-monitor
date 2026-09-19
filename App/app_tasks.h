/**
  ******************************************************************************
  * @file    app_tasks.h
  * @brief   六个 FreeRTOS 任务的划分、通信与对外接口。
  *
  * ── 任务划分 ────────────────────────────────────────────────────────
  *
  *   任务          周期      职责
  *   TaskAcquire   50ms      读 ADC+DMA 缓冲、滑动平均、发队列
  *   TaskSensor    1000ms    读 SHT30（软件 I²C，慢），发队列
  *   TaskProcess   事件驱动   等两路数据齐 → 越限判定 → 发显示队列 + 置报警位
  *   TaskDisplay   500ms     刷 OLED（持总线锁最久，所以优先级最低）
  *   TaskComm      事件驱动   打包 + CRC16 + UART DMA 发送
  *   TaskMonitor   500ms     喂 IWDG、查各任务心跳、驱动蜂鸣器
  *
  * ── 为什么要用 RTOS 而不是裸机大循环 ────────────────────────────────
  *
  * 六个环节的周期差 20 倍（50ms vs 1000ms）。裸机大循环只能串行执行，
  * 读 SHT30 的 15ms 会把 50ms 的采集周期直接拖垮；把慢操作塞进中断又
  * 会让中断变得又长又难维护。RTOS 让每个环节按自己的周期独立跑，
  * 谁慢谁自己慢，不拖累别人。
  ******************************************************************************
  */

#ifndef __APP_TASKS_H
#define __APP_TASKS_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

/* ==========================================================================
 * 数据
 * ========================================================================== */

/** 报警标志位。多个条件可能同时成立，所以用位域而不是枚举 */
#define ALARM_TEMP_HIGH    (1u << 0)
#define ALARM_TEMP_LOW     (1u << 1)
#define ALARM_HUMI_HIGH    (1u << 2)
#define ALARM_HUMI_LOW     (1u << 3)
#define ALARM_LIGHT_HIGH   (1u << 4)
#define ALARM_LIGHT_LOW    (1u << 5)
#define ALARM_SENSOR_ERR   (1u << 6)   /**< SHT30 读失败（通信错或 CRC 错） */

/** 一路完整的采样数据，在任务之间用队列传递 */
typedef struct
{
    int16_t  temp_x100;   /**< 温度 ×100。ALARM_SENSOR_ERR 置位时无意义 */
    int16_t  humi_x100;   /**< 湿度 ×100。同上 */
    uint16_t light;       /**< 光照 ADC 原始值 */
    uint16_t pot;         /**< 电位器 ADC 原始值 */
    uint8_t  alarm;       /**< ALARM_xxx 的按位或 */
} app_sample_t;

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

/**
 * @brief 初始化外设、创建所有队列/事件组/互斥锁，并创建六个任务。
 *
 * 调用位置：`MX_FREERTOS_Init()` 的 `USER CODE BEGIN RTOS_THREADS` 区块内。
 * 必须在 `osKernelStart()` **之前**调用（此时只能创建对象，不能获取锁）。
 *
 * ★ 唯一例外是 OLED：`ssd1306_init()` 需要拿总线锁，而 **FreeRTOS 不允许
 *   在调度器启动前获取互斥锁**，所以它放在 TaskDisplay 的第一轮循环里执行。
 */
void app_init(void);

/**
 * @brief 按键中断的处理入口，从 `HAL_GPIO_EXTI_Callback()` 里调用。
 *
 * 内部**只做一件事**：通过任务通知唤醒 TaskMonitor，然后立刻返回。
 * 业务逻辑（消音、换页）全部在任务里做 —— 中断里绝不写业务逻辑，
 * 这是实时系统的铁律：中断执行时间必须尽可能短，否则会挤压所有任务。
 */
void app_key_isr(void);

/**
 * @brief 读取"蜂鸣器是否已消音"。
 *
 * ★ 这是一个**跨任务共享的字节**，刻意不加锁：Cortex-M3 上 8 位读写是
 *   原子的，不存在读到"半个值"的可能。最坏情况是读到上一次的状态，
 *   对一个"静音开关"而言完全可以接受。
 *   如果将来这里换成多字节数据（比如一个结构体），就必须改成加锁或用队列。
 */
bool app_is_muted(void);

/** @brief 累计的按键次数（用于上位机/调试观察） */
uint32_t app_get_key_count(void);

#endif /* __APP_TASKS_H */
