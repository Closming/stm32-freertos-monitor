/**
  ******************************************************************************
  * @file    app_ssd1306.c
  * @brief   SSD1306 OLED 驱动实现。
  ******************************************************************************
  */

#include "app_ssd1306.h"
#include "app_i2c.h"
#include "app_font.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * 命令
 * ========================================================================== */

/* SSD1306 的一字节"控制字节"：告诉屏后面跟的是命令还是显存数据。
 *
 * ★★★ 这两个字节**不能在同一个 I²C 事务里混用** —— 本项目最大的一个坑。★★★
 *
 *   控制字节的 bit7 是 Co（Continuation，续传位）：
 *     Co = 0  ⇒ 从这一字节起**直到 STOP 为止**，后面所有字节都是同一种
 *               类型，屏不会再去读控制字节。
 *     Co = 1  ⇒ 只有**一个**字节是这种类型，之后必须再给一个控制字节。
 *
 *   下面两个常量都是 Co=0 的写法。所以曾经这样发显存：
 *
 *      0x00 | 0x21 0x00 0x7F | 0x22 p0 p1 | 0x40 | <128 字节显存> | STOP
 *                                            ↑ 这里已经不是控制字节了
 *
 *   开头的 0x00 声明了"后面全是命令"，屏就**真的**把 0x40 当命令执行了
 *   （0x40 = Set Display Start Line = 0），紧跟其后的 128 字节显存
 *   也一并被当成命令 —— GDDRAM 一个字节都没写进去。
 *
 *   ★ 这就是"发 0xAE/0xAF 屏幕跟着亮灭、刷显存却纹丝不动"的根因：
 *     ssd1306_send_cmd_op() 只发命令，结构本来就是对的；而刷显存要
 *     **先命令后数据**，中间少了那个 STOP。
 *     屏上于是永远显示上电时的随机显存内容 —— 也就是雪花。
 *
 *   ⚠️ 所以 ssd1306_send_data_op() **必须**拆成两个事务。
 *      另一个等价写法是全程用 Co=1（每字节前面加 0x80）续传，
 *      能塞进一个事务，但可读性差得多，不推荐。
 *
 *   （排查时绕的弯路：一度怀疑模块其实是 SH1106、怀疑上拉电阻不够、
 *     怀疑 SDA 被拉低。其实命令通道能稳定跑完 26 字节初始化序列，
 *     就说明电气层、器件地址、时序全都是好的，不该再去动硬件。
 *     教训：**先怀疑协议用法，再怀疑硬件**。） */
#define SSD1306_CTRL_CMD     0x00u   /**< Co=0, D/C#=0：后续直到 STOP 全是命令 */
#define SSD1306_CTRL_DATA    0x40u   /**< Co=0, D/C#=1：后续直到 STOP 全是显存数据 */

/* 寻址模式：水平寻址。设好列范围和页范围后，数据会按列自动递增、
   列满了自动换下一页 —— 所以整块显存可以一次性连续发出去。 */
#define SSD1306_CMD_ADDR_MODE        0x20u
#define SSD1306_ADDR_MODE_HORIZONTAL 0x00u

#define SSD1306_CMD_COL_ADDR 0x21u   /**< 设置列范围：后跟起始列、结束列 */
#define SSD1306_CMD_PAGE_ADDR 0x22u  /**< 设置页范围：后跟起始页、结束页 */

/* ==========================================================================
 * 初始化序列
 *
 * 每一项的含义都注在下面。这一串数字看着像天书，但都是从数据手册的
 * "Application Example" 直接搬过来的推荐值 —— 自己拍脑袋改只会得到
 * 一块花屏或者黑屏。
 * ========================================================================== */
static const uint8_t s_init_seq[] =
{
    0xAEu,              /* 关显示（配置期间先关掉，避免看到中间状态） */
    0x20u, 0x00u,       /* 寻址模式 = 水平寻址 */
    0xB0u,              /* 页起始地址 = 0 */
    0xC8u,              /* COM 扫描方向 = 反向（屏幕上下不颠倒） */
    0x00u,              /* 列低 4 位地址 = 0 */
    0x10u,              /* 列高 4 位地址 = 0 */
    0x40u,              /* 显示起始行 = 0 */
    0x81u, 0xCFu,       /* 对比度 = 0xCF */
    0xA1u,              /* 段重映射：左右不镜像 */
    0xA6u,              /* 正常显示（不做反色） */
    0xA8u, 0x3Fu,       /* 多路复用比 = 63 → 64 行 */
    0xA4u,              /* 显示内容来自显存 */
    0xD3u, 0x00u,       /* 显示偏移 = 0 */
    0xD5u, 0x80u,       /* 时钟分频 / 振荡频率 */
    0xD9u, 0xF1u,       /* 预充电周期 */
    0xDAu, 0x12u,       /* COM 引脚配置 = 交替，适用于 128×64 */
    0xDBu, 0x40u,       /* VCOMH 电压 */
    0x8Du, 0x14u,       /* ★ 电荷泵使能 —— 少了这条屏完全不亮 */
    0xAFu,              /* 开显示 */
};

/* ==========================================================================
 * 模块内部状态（先声明后用）
 * ========================================================================== */

/** 显存。8 页 × 128 列，格式与屏原生格式一致，刷新时可直接搬运。 */
static uint8_t s_gram[SSD1306_PAGE_COUNT][SSD1306_WIDTH];

static uint32_t s_flush_error_count = 0u;

/* ==========================================================================
 * 底层：命令与数据的事务实现体
 *
 * 这些都是 i2c_transaction() 的回调，**在持有总线锁的状态下执行**。
 * ========================================================================== */

/** 发送若干字节命令 */
typedef struct
{
    const uint8_t *data;
    uint16_t       len;
} ssd_cmd_ctx_t;

static bool ssd1306_send_cmd_op(void *ctx)
{
    ssd_cmd_ctx_t *c = (ssd_cmd_ctx_t *)ctx;
    uint16_t i;

    i2c_bus_start();
    if (!i2c_bus_write((uint8_t)(SSD1306_I2C_ADDR | 0x00u)))
    {
        i2c_bus_stop();
        return false;
    }
    if (!i2c_bus_write(SSD1306_CTRL_CMD))
    {
        i2c_bus_stop();
        return false;
    }

    for (i = 0u; i < c->len; i++)
    {
        if (!i2c_bus_write(c->data[i]))
        {
            i2c_bus_stop();
            return false;
        }
    }

    i2c_bus_stop();
    return true;
}

/** 写入一片显存：先设好列范围/页范围，再连续灌数据 */
typedef struct
{
    uint8_t        page_start;
    uint8_t        page_end;
    const uint8_t *data;
    uint16_t       len;
} ssd_data_ctx_t;

static bool ssd1306_send_data_op(void *ctx)
{
    ssd_data_ctx_t *c = (ssd_data_ctx_t *)ctx;
    uint16_t i;

    i2c_bus_start();
    if (!i2c_bus_write((uint8_t)(SSD1306_I2C_ADDR | 0x00u)))
    {
        i2c_bus_stop();
        return false;
    }

    /* ---- 事务 1：定位（纯命令）----
       控制字节 0x00 声明"后面直到 STOP 全是命令"，所以这一串必须以
       STOP 收尾，否则控制字节的语义会一直延续到事务 2 去（见文件头说明）。 */
    if (!i2c_bus_write(SSD1306_CTRL_CMD) ||
        !i2c_bus_write(SSD1306_CMD_COL_ADDR) ||     /* 0x21 起始列 */
        !i2c_bus_write(0x00u) ||
        !i2c_bus_write((uint8_t)(SSD1306_WIDTH - 1u)) ||
        !i2c_bus_write(SSD1306_CMD_PAGE_ADDR) ||    /* 0x22 起始页 */
        !i2c_bus_write(c->page_start) ||
        !i2c_bus_write(c->page_end))
    {
        i2c_bus_stop();
        return false;
    }

    i2c_bus_stop();     /* ★★★ 关键：靠这个 STOP 切断控制字节的作用范围 ★★★ */

    /* ---- 事务 2：显存数据 ----
       重新起一次事务。这里的总线锁一直握着（本函数整体在 i2c_transaction()
       的临界区里），所以两个事务之间不会有别的任务插进来，定位不会失效。 */
    i2c_bus_start();
    if (!i2c_bus_write((uint8_t)(SSD1306_I2C_ADDR | 0x00u)))
    {
        i2c_bus_stop();
        return false;
    }

    /* 控制字节 0x40 声明"后面直到 STOP 全是数据" */
    if (!i2c_bus_write(SSD1306_CTRL_DATA))
    {
        i2c_bus_stop();
        return false;
    }

    for (i = 0u; i < c->len; i++)
    {
        if (!i2c_bus_write(c->data[i]))
        {
            i2c_bus_stop();
            return false;
        }
    }

    i2c_bus_stop();
    return true;
}

/* ==========================================================================
 * 上层封装
 * ========================================================================== */

static bool ssd1306_send_cmd(const uint8_t *data, uint16_t len)
{
    ssd_cmd_ctx_t ctx;

    ctx.data = data;
    ctx.len  = len;

    return i2c_transaction(ssd1306_send_cmd_op, &ctx, I2C_TIMEOUT_MS);
}

void ssd1306_init(void)
{
    ssd1306_clear();

    if (!ssd1306_send_cmd(s_init_seq, (uint16_t)sizeof(s_init_seq)))
    {
        /* 屏初始化不了，后面所有显示都没意义。早失败早发现。 */
        Error_Handler();
    }

    (void)ssd1306_flush();
}

void ssd1306_clear(void)
{
    memset(s_gram, 0x00, sizeof(s_gram));
}

bool ssd1306_flush(void)
{
    ssd_data_ctx_t ctx;
    uint8_t        page;
    bool           all_ok = true;

    /* ★ 分片发送：每次事务写 SSD1306_FLUSH_CHUNK_PAGES 页，
       事务之间**释放总线锁**，让别的任务有机会插进来。
       整屏 1024 字节一次发完的话，锁要被占住约 150ms。 */
    for (page = 0u; page < SSD1306_PAGE_COUNT; page += SSD1306_FLUSH_CHUNK_PAGES)
    {
        uint8_t end = (uint8_t)(page + SSD1306_FLUSH_CHUNK_PAGES - 1u);

        if (end >= SSD1306_PAGE_COUNT)
        {
            end = (uint8_t)(SSD1306_PAGE_COUNT - 1u);
        }

        ctx.page_start = page;
        ctx.page_end   = end;
        ctx.data       = &s_gram[page][0];
        ctx.len        = (uint16_t)((uint16_t)(end - page + 1u) * SSD1306_WIDTH);

        if (!i2c_transaction(ssd1306_send_data_op, &ctx, I2C_TIMEOUT_MS))
        {
            s_flush_error_count++;
            all_ok = false;
            /* 一个分片失败不影响其余分片 —— 屏幕可能只有一部分没刷新，
               但整体仍然可用，比整屏放弃要好。 */
        }
    }

    return all_ok;
}

/* ==========================================================================
 * 画图
 * ========================================================================== */

void ssd1306_draw_text(uint8_t page, uint8_t col, const char *text)
{
    if (text == NULL || page >= SSD1306_PAGE_COUNT)
    {
        return;
    }

    while (*text != '\0' && col < SSD1306_WIDTH)
    {
        uint8_t code = (uint8_t)(*text);
        uint8_t x;

        /* 字库范围外的字符（比如 UTF-8 中文的第一个字节）按空格画，
           这样最多显示成空白，不会因为越界读 flash 而画出乱码。 */
        if (code < FONT_FIRST_CHAR || code > FONT_LAST_CHAR)
        {
            code = (uint8_t)' ';
        }

        {
            const uint8_t *glyph = &g_font5x7[(uint16_t)(code - FONT_FIRST_CHAR) * FONT_CHAR_WIDTH];

            for (x = 0u; x < FONT_CHAR_WIDTH; x++)
            {
                if ((uint8_t)(col + x) < SSD1306_WIDTH)
                {
                    s_gram[page][col + x] = glyph[x];
                }
            }
        }

        col = (uint8_t)(col + FONT_CHAR_SPACING);
        text++;
    }
}

void ssd1306_printf(uint8_t page, const char *fmt, ...)
{
    char    buf[24];   /* 128 像素 / 6 像素每字符 ≈ 21 字符，24 够用 */
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ssd1306_draw_text(page, 0u, buf);
}

void ssd1306_draw_text_centered(uint8_t page, const char *text)
{
    uint8_t len;

    if (text == NULL)
    {
        return;
    }

    len = (uint8_t)strlen(text);

    if ((uint16_t)len * FONT_CHAR_SPACING >= SSD1306_WIDTH)
    {
        ssd1306_draw_text(page, 0u, text);
        return;
    }

    ssd1306_draw_text(
        page,
        (uint8_t)((SSD1306_WIDTH - (uint16_t)len * FONT_CHAR_SPACING) / 2u),
        text);
}

uint32_t ssd1306_get_flush_error_count(void)
{
    return s_flush_error_count;
}
