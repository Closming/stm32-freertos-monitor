/**
  ******************************************************************************
  * @file    app_sht30.c
  * @brief   SHT30 驱动实现。
  ******************************************************************************
  */

#include "app_sht30.h"
#include "app_i2c.h"
#include "app_uart.h"
#include "cmsis_os2.h"

/* ==========================================================================
 * 命令与常量
 * ========================================================================== */

/** 单次测量，高重复性，**关闭时钟拉伸**。
 *
 * 为什么关拉伸：打开拉伸的话传感器在转换期间会把 SCL 拉住不放，
 * 软件模拟 I²C 必须一直等它——逻辑更复杂，而且要从总线层面处理。
 * 关掉之后主机自己等固定时间即可，行为可预期。
 *
 * ★★ 2026-09-26 修：这里原本写的是 **0x2C06**，那恰恰是**开启**时钟拉伸的
 *    那一个（数据手册里单次测量分两组：0x2Cxx = stretching enabled，
 *    0x24xx = stretching disabled，低字节 0x06 就是"高重复性 + 使能拉伸"）。
 *    注释写着要关，命令字却在开 —— 意图和实现相反。
 *
 *    后果：传感器收到命令后把 SCL 拉住约 15ms，而软件 I²C 的
 *    scl_release_and_wait() 守卫只有 I2C_STRETCH_LIMIT≈1ms，必然超时；
 *    超时后 i2c_bus_stop() 又丢弃了返回值照常往下走 → 发出的是无效 STOP →
 *    总线卡在脏状态，**共总线上的 SSD1306 一起失联**，上电即黑屏。
 *    详见 README「遇到的问题与解决」。 */
#define SHT30_CMD_MEASURE_HIGH  0x2400u

/** 高重复性单次测量的转换时间上限（数据手册给 15ms，留余量取 20ms） */
#define SHT30_CONVERSION_MS     20u

/** 读回的 6 个字节：温度 2 + 温度CRC 1 + 湿度 2 + 湿度CRC 1 */
#define SHT30_READ_LEN          6u

/** 每一次 CRC 校验**覆盖**的字节数 —— 只有这 2 个数据字节。
 *
 * ★★ 2026-09-26 修：这里原本是 **3**，注释还写着"2 字节数据 + 1 字节校验"——
 *    把**校验值本身**也算进了被校验的区间，理解反了。
 *
 *    数据手册的规定是：器件对 2 个数据字节算 CRC，把结果作为第 3 字节发出来。
 *    主机这边要做的只是「算前 2 字节，和第 3 字节比」。
 *
 *    后果：CRC-8 有个性质 —— 若 crc8(A,B) == C，则把 C 再异或进去后结果必为 0，
 *    即 crc8(A,B,C) 恒等于 0x00。而代码拿这个 0 去和 raw[2]（真实校验值）比，
 *    **除非校验值恰好是 0x00，否则 100% 不相等** —— 于是每一次读取都判失败，
 *    但读到的数据其实一直是对的。
 *
 *    这个 bug 的表象极具误导性：ALARM_SENSOR_ERR 常亮、OLED 恒显 --.--，
 *    看着像接线/上拉/时钟拉伸，实际全是软件。当天一路查了供电、上拉、
 *    时钟拉伸、锁竞争才走到这里。教训：**CRC 失败先怀疑校验区间，再怀疑信号。** */
#define SHT30_CRC_BLOCK         2u

/* ==========================================================================
 * 模块内部状态
 * ========================================================================== */
static uint32_t s_crc_error_count = 0u;

/* ==========================================================================
 * CRC8
 *
 * 多项式 x^8 + x^5 + x^4 + 1，写成位形式是 0x31，初值 0xFF。
 * 这是 SHT30 数据手册规定的算法，**不是**常见的 CRC-8/MAXIM（那个多项式是 0x31
 * 但反射形式不同，算出来结果不一样）。实现成逐位法，只有 2 字节要算，不必查表。
 * ========================================================================== */
static uint8_t sht30_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFFu;
    uint8_t i;
    uint8_t bit;

    for (i = 0u; i < len; i++)
    {
        crc ^= data[i];
        for (bit = 0u; bit < 8u; bit++)
        {
            if ((crc & 0x80u) != 0u)
            {
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

/** CRC 失败时把原始 6 字节打出来，只打前几次。
 *
 * 为什么要这个：nack=0 / timeout=0 之后，唯一还说不清的就是 crc。
 * 「读到了 6 个字节但校验对不上」有两种截然不同的成因，光看计数器分不出：
 *
 *   全 0x00 / 全 0xFF  → 器件根本没返回有效数据
 *                        （命令没生效，或者转换还没完成就去读了）
 *   数值看着像回事      → 数据本身是"合理"的，那问题在采样点偏了，
 *                        或者 CRC 算法与器件实际用的那套不一致
 *
 * 只打前 3 次：TaskSensor 一秒读一次，不加限制会淹掉串口。
 * ★ 诊断用，问题定位后可以整段删掉。 */
static void sht30_log_raw_crc_fail(const uint8_t *raw, uint32_t err_seq)
{
    if (err_seq > 3u)
    {
        return;
    }

    uart_log("[SHT] crc fail #%u raw=%02X %02X %02X %02X %02X %02X"
             " calcT=%02X calcH=%02X\r\n",
             (unsigned)err_seq,
             (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2],
             (unsigned)raw[3], (unsigned)raw[4], (unsigned)raw[5],
             (unsigned)sht30_crc8(&raw[0], SHT30_CRC_BLOCK),
             (unsigned)sht30_crc8(&raw[3], SHT30_CRC_BLOCK));
}

/* ==========================================================================
 * 事务 1：发测量命令
 * ========================================================================== */

static bool sht30_send_cmd_op(void *ctx)
{
    (void)ctx;

    i2c_bus_start();
    if (!i2c_bus_write((uint8_t)(SHT30_I2C_ADDR | 0x00u)))   /* 地址 + 写 */
    {
        i2c_bus_stop();
        return false;
    }
    if (!i2c_bus_write((uint8_t)(SHT30_CMD_MEASURE_HIGH >> 8)))
    {
        i2c_bus_stop();
        return false;
    }
    if (!i2c_bus_write((uint8_t)(SHT30_CMD_MEASURE_HIGH & 0xFFu)))
    {
        i2c_bus_stop();
        return false;
    }
    i2c_bus_stop();
    return true;
}

/* ==========================================================================
 * 事务 2：读 6 个字节
 * ========================================================================== */

typedef struct
{
    uint8_t raw[SHT30_READ_LEN];
} sht30_read_ctx_t;

static bool sht30_fetch_op(void *ctx)
{
    sht30_read_ctx_t *c = (sht30_read_ctx_t *)ctx;
    uint8_t i;

    i2c_bus_start();
    if (!i2c_bus_write((uint8_t)(SHT30_I2C_ADDR | 0x01u)))   /* 地址 + 读 */
    {
        i2c_bus_stop();
        return false;
    }

    for (i = 0u; i < SHT30_READ_LEN; i++)
    {
        /* 最后一个字节回 NACK —— 告诉从机"我读完了"。
           这是 I²C 的礼貌，也是主机唯一能主动结束读取的方式。 */
        c->raw[i] = i2c_bus_read(i < (SHT30_READ_LEN - 1u));
    }

    i2c_bus_stop();
    return true;
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

bool sht30_read(sht30_data_t *out)
{
    sht30_read_ctx_t ctx;
    uint16_t temp_raw;
    uint16_t humi_raw;

    if (out == NULL)
    {
        return false;
    }

    /* ---- 事务 1：下命令 ---- */
    if (!i2c_transaction(sht30_send_cmd_op, NULL, I2C_TIMEOUT_MS))
    {
        return false;
    }

    /* ---- 等待转换。★ 此时**没有**持有总线锁 ---- */
    osDelay(SHT30_CONVERSION_MS);

    /* ---- 事务 2：读结果 ---- */
    if (!i2c_transaction(sht30_fetch_op, &ctx, I2C_TIMEOUT_MS))
    {
        return false;
    }

    /* 温度在前，湿度在后，每个都是 数据2字节 + CRC1字节 */
    if (sht30_crc8(&ctx.raw[0], SHT30_CRC_BLOCK) != ctx.raw[2])
    {
        s_crc_error_count++;
        sht30_log_raw_crc_fail(ctx.raw, s_crc_error_count);
        return false;
    }
    if (sht30_crc8(&ctx.raw[3], SHT30_CRC_BLOCK) != ctx.raw[5])
    {
        s_crc_error_count++;
        sht30_log_raw_crc_fail(ctx.raw, s_crc_error_count);
        return false;
    }

    temp_raw = (uint16_t)(((uint16_t)ctx.raw[0] << 8) | ctx.raw[1]);
    humi_raw = (uint16_t)(((uint16_t)ctx.raw[3] << 8) | ctx.raw[4]);

    /* 数据手册的转换公式：
       温度 = -45 + 175 × (raw / 65535)
       湿度 =       100 × (raw / 65535)
       这里直接算成 ×100 的整数，避免在 MCU 上引入浮点。
       17500 × 65535 ≈ 1.15e9，int32 装得下，不会溢出。 */

    out->temp_x100 = (int16_t)(-4500 + (int32_t)((int32_t)17500 * temp_raw) / 65535);
    out->humi_x100 = (int16_t)((int32_t)((int32_t)10000 * humi_raw) / 65535);

    return true;
}

uint32_t sht30_get_crc_error_count(void)
{
    return s_crc_error_count;
}
