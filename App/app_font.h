/**
  ******************************************************************************
  * @file    app_font.h
  * @brief   5×7 点阵字库接口。
  *
  * ★ 由 Tools/gen_font.py 自动生成，不要手工编辑。
  ******************************************************************************
  */

#ifndef __APP_FONT_H
#define __APP_FONT_H

#include <stdint.h>

#define FONT_FIRST_CHAR   0x20u
#define FONT_LAST_CHAR    0x7Fu
#define FONT_CHAR_WIDTH   5u   /**< 字形宽（像素） */
#define FONT_CHAR_HEIGHT  7u   /**< 字形高（像素） */
#define FONT_CHAR_SPACING 6u   /**< 含右侧留白的推进宽度 */

/** 字库表。下标 = ASCII 码 - FONT_FIRST_CHAR，每字符 5 字节。 */
extern const uint8_t g_font5x7[];

/** 度符号的字符串写法。
 *
 * ★ 必须像这样**分段**拼接：
 *       ssd1306_printf(0, "TEMP 23.6" FONT_DEG "C");
 *
 *   写成 "TEMP 23.6\x7FC" 是错的：C 的十六进制转义是**贪婪匹配**，
 *   编译器会把 \x7FC 整个当成一个转义序列（值 0x7FC），
 *   结果是报错或者字符被截断。分段写法里转义在引号处自然结束。
 */
#define FONT_DEG "\x7F"

#endif /* __APP_FONT_H */
