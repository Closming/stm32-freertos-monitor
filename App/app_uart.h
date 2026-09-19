/**
  ******************************************************************************
  * @file    app_uart.h
  * @brief   串口：printf 重定向 + 帧的 DMA 发送 + 任务安全的日志。
  *
  * 为什么串口要"最早做通"：它是后面所有调试的眼睛。没有它，调传感器、
  * 调 FreeRTOS 都只能靠猜。方案里把它排在 Day 3，就是这个原因。
  *
  * ── 三条发送路径，用途不同 ──────────────────────────────────────────
  *
  *   uart_log()        任务里打日志。**推荐用这个**，不要直接 printf。
  *   printf()          通过 fputc 重定向，逐字节阻塞发送。只在
  *                     调度器还没启动时（初始化阶段）用。
  *   uart_send_frame() 发协议帧，走 DMA，发送期间不占 CPU。
  *
  * ★ 前两者和第三者共用同一个 USART1，所以**用同一把互斥锁串行化**。
  *   否则日志和帧会在物理线路上交错，上位机收到的是一堆乱码。
  ******************************************************************************
  */

#ifndef __APP_UART_H
#define __APP_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

/**
 * @brief 创建串口互斥锁与 DMA 完成信号量。
 *
 * 必须在调度器启动**之前**调用（放在 main 里 MX_USART1_UART_Init() 之后）。
 */
void uart_init(void);

/**
 * @brief 任务安全的日志输出，用法同 printf。
 *
 * 内部先 vsnprintf 到栈上缓冲区，再整体加锁发送 —— 一次日志只拿一次锁，
 * 比 printf 逐字符拿锁高效得多，也不会被别的任务的输出插进来。
 *
 * 超过 UART_LOG_BUF_SIZE 的部分会被截断（不会溢出）。
 */
void uart_log(const char *fmt, ...);

/**
 * @brief 用 DMA 发送一帧协议数据。
 *
 * 会阻塞等待发送完成（靠 DMA 完成中断释放的信号量），但等待期间
 * **不占用 CPU 做忙等** —— 这正是用 DMA 而不是轮询发送的意义。
 *
 * 同一时刻只允许一帧在飞：本函数内部持有串口互斥锁。
 *
 * @param buf 数据首地址。**函数返回后即可复用**（返回前一定已发完）
 * @param len 字节数
 * @return true = 已发出；false = 拿锁超时或 HAL 报错（该帧被丢弃）
 */
bool uart_send_frame(const uint8_t *buf, uint16_t len);

#endif /* __APP_UART_H */
