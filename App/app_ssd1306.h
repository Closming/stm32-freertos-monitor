/**
  ******************************************************************************
  * @file    app_ssd1306.h
  * @brief   SSD1306 OLED 驱动（128×64，I²C）。
  *
  * ── 这个驱动为什么自己写而不用现成库 ────────────────────────────────
  *
  * GitHub 上有大量现成的 ssd1306 库可以直接移植，但它们的 I²C 收发
  * 都是自己实现的，绕过了本项目的 `i2c_transaction()` 封装 ——
  * 也就是说，**用了库就等于把互斥锁废掉了**，OLED 和传感器照样会打架。
  *
  * 自己写的最小实现只有两百来行，每一处 I²C 收发都走总线锁，
  * 而且面试时能一行行解释。这笔账是划算的。
  *
  * ── 显存布局 ────────────────────────────────────────────────────────
  *
  * 内部有一块 8×128 的显存，和屏的原生格式一致：
  *   第 p 页第 x 列的那一字节 = 屏幕第 x 列、第 p*8 ~ p*8+7 行的 8 个像素，
  *   bit0 对应最上面那一行。
  *
  * 所以"画点"是改显存的某一位，"刷新"是把整块显存原样搬给屏。
  * 这个布局的好处是刷新时**不需要任何逐位转换**。
  ******************************************************************************
  */

#ifndef __APP_SSD1306_H
#define __APP_SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

/**
 * @brief 发送初始化序列。
 *
 * 必须在 i2c_init() **之后**调用（要靠总线锁）。
 * ★ 初始化失败**不再致命**：屏不接时系统照常运行，只是把 s_present 置 false。
 * 用 ssd1306_is_present() 查询结果，false 时调用方应跳过渲染与刷新。
 * （不要改回 Error_Handler() —— 那会 __disable_irq() 卡死，连串口一起带走，
 *   使「屏没接」和「板子没跑起来」无法区分，详见 app_ssd1306.c。）
 */
void ssd1306_init(void);

/** @brief 屏是否就绪。ssd1306_init() 之后有效；false = 屏没应答。 */
bool ssd1306_is_present(void);

/** @brief 清空显存（只改内存，不动屏；要看见效果得再调 ssd1306_flush） */
void ssd1306_clear(void);

/**
 * @brief 把显存刷到屏上。
 *
 * 分 SSD1306_FLUSH_CHUNK_PAGES 页一片、分多次 I²C 事务发送，
 * 每次事务之间释放总线锁。见 app_config.h 里对该参数的说明。
 *
 * 整屏约需 150ms（60kHz 软件 I²C 估算），所以**不要频繁调用** ——
 * 这也是显示任务周期定为 500ms 而不是 200ms 的原因。
 *
 * @return true = 全部分片都发送成功
 */
bool ssd1306_flush(void);

/**
 * @brief 在指定页、指定列开始画一段文本。
 *
 * @param page 页号 0~7（每页 8 像素高，正好放得下 7 像素的字）
 * @param col  起始列 0~127
 * @param text 文本。字库只覆盖 0x20~0x7F，范围外的字符按空格处理，
 *             所以**不要直接放 UTF-8 中文字符串**（会被拆成多个乱码字）。
 */
void ssd1306_draw_text(uint8_t page, uint8_t col, const char *text);

/**
 * @brief 在指定页从第 0 列开始画一行格式化文本。
 *
 * @param page 页号 0~7
 * @param fmt  printf 风格的格式串
 */
void ssd1306_printf(uint8_t page, const char *fmt, ...);

/**
 * @brief 把一段文本水平居中画在指定页。
 *
 * 用于显示 "NO SENSOR" 这类提示，比左对齐好看。
 */
void ssd1306_draw_text_centered(uint8_t page, const char *text);

/** @brief 累计刷新失败的次数（拿不到锁或 I²C 报错）。正常应恒为 0 */
uint32_t ssd1306_get_flush_error_count(void);

#endif /* __APP_SSD1306_H */
