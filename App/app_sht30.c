/**
  ******************************************************************************
  * @file    app_sht30.c
  * @brief   SHT30 驱动实现。
  ******************************************************************************
  */

#include "app_sht30.h"
#include "app_i2c.h"
#include "cmsis_os2.h"

/* ==========================================================================
 * 命令与常量
 * ========================================================================== */

/** 单次测量，高重复性，**关闭时钟拉伸**。
 *
 * 为什么关拉伸：打开拉伸的话传感器在转换期间会把 SCL 拉住不放，
 * 软件模拟 I²C 必须一直等它——逻辑更复杂，而且要从总线层面处理。
 * 关掉之后主机自己等固定时间即可，行为可预期。 */
#define SHT30_CMD_MEASURE_HIGH  0x2C06u

/** 高重复性单次测量的转换时间上限（数据手册给 15ms，留余量取 20ms） */
#define SHT30_CONVERSION_MS     20u

/** 读回的 6 个字节：温度 2 + 温度CRC 1 + 湿度 2 + 湿度CRC 1 */
#define SHT30_READ_LEN          6u

/** 每一次 CRC 校验覆盖的字节数（2 字节数据 + 1 字节校验） */
#define SHT30_CRC_BLOCK         3u

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
        return false;
    }
    if (sht30_crc8(&ctx.raw[3], SHT30_CRC_BLOCK) != ctx.raw[5])
    {
        s_crc_error_count++;
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
