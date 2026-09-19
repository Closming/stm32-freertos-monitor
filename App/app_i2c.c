/**
  ******************************************************************************
  * @file    app_i2c.c
  * @brief   GPIO 软件模拟 I²C + 总线互斥锁。
  ******************************************************************************
  */

#include "app_i2c.h"
#include "cmsis_os2.h"

/* ==========================================================================
 * 模块内部状态
 *
 * ★ 必须放在最前面：下面的 i2c_bus_write() 会更新 s_nack_count，
 *   C 里"先用后声明"是编译错误（不像函数有隐式声明），
 *   挪到后面会直接编不过。
 * ========================================================================== */
static osMutexId_t s_i2c_mutex     = NULL;
static uint32_t    s_timeout_count = 0u;   /* 拿锁超时次数，正常应恒为 0 */
static uint32_t    s_nack_count    = 0u;   /* 器件无应答次数 */

/* ==========================================================================
 * 硬件层：两根线的读写
 *
 * 两条线都配成**开漏输出**（Open-Drain），这是 I²C 的电气要求：
 *   - 写 0 → 三极管导通，把线拉到 GND（强低）
 *   - 写 1 → 三极管截止，线被外部上拉电阻拉到 3.3V（弱高，也就是"释放"）
 *
 * ★ 关于读回：STM32F1 的 GPIO 在开漏输出模式下，输入施密特触发器
 *   **仍然是接通的**，所以 IDR（HAL_GPIO_ReadPin 读的就是它）反映的是
 *   引脚上的**真实电平**，而不是我们写出去的值。
 *
 *   所以读 SDA 不需要切换成输入模式，只要先"释放"（写 1），
 *   再读 IDR 拿到的就是从机拉的电平。这条是很多软件 I²C 代码里
 *   切换输入/输出模式的根源——F1 上可以省掉，省掉后时序也更干净。
 *
 *   ⚠️ 万一硬件上发现"永远读不到 ACK"，第一个要怀疑的就是这条假设。
 *   届时的验证方法：释放 SDA 后用万用表量引脚电压，若确实被外部拉到
 *   3.3V，而 IDR 仍读 0，就改用切换输入模式（HAL_GPIO_Init）的方案。
 * ========================================================================== */

static inline void scl_release(void)
{
    HAL_GPIO_WritePin(I2C_SCL_GPIO_Port, I2C_SCL_Pin, GPIO_PIN_SET);
}

static inline void scl_low(void)
{
    HAL_GPIO_WritePin(I2C_SCL_GPIO_Port, I2C_SCL_Pin, GPIO_PIN_RESET);
}

static inline void sda_release(void)
{
    HAL_GPIO_WritePin(I2C_SDA_GPIO_Port, I2C_SDA_Pin, GPIO_PIN_SET);
}

static inline void sda_low(void)
{
    HAL_GPIO_WritePin(I2C_SDA_GPIO_Port, I2C_SDA_Pin, GPIO_PIN_RESET);
}

static inline void sda_write(uint8_t bit)
{
    if (bit) { sda_release(); } else { sda_low(); }
}

/** @brief 读 SDA 真实电平。前提：调用前必须已释放 SDA。 */
static inline uint8_t sda_read(void)
{
    return (HAL_GPIO_ReadPin(I2C_SDA_GPIO_Port, I2C_SDA_Pin) == GPIO_PIN_SET) ? 1u : 0u;
}

/** @brief 读 SCL 真实电平，用于判断从机是否在拉长时钟（clock stretching） */
static inline uint8_t scl_read(void)
{
    return (HAL_GPIO_ReadPin(I2C_SCL_GPIO_Port, I2C_SCL_Pin) == GPIO_PIN_SET) ? 1u : 0u;
}

/**
 * @brief 释放 SCL 并等待它真正变高（兼容从机的时钟拉伸）。
 *
 * 有些从机（尤其 EEPROM、部分传感器）会在忙的时候把 SCL 拉低不放，
 * 这叫时钟拉伸。主机必须等它释放才能继续，否则会丢位。
 * 但不能无限等——从机挂了的话会把整个任务吊死在这里，
 * 所以有个上限，超了就放弃本次事务。
 *
 * @return true = SCL 已变高；false = 等超时，总线异常
 */
static bool scl_release_and_wait(void)
{
    uint32_t guard = I2C_STRETCH_LIMIT;

    scl_release();
    while (scl_read() == 0u)
    {
        if (--guard == 0u)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 半周期延时。
 *
 * 用忙等而不是 osDelay：I²C 的位间隔是微秒级，osDelay 最小粒度是
 * 一个 tick（默认 1ms），用它等于把总线降到 500Hz。
 *
 * 频率不追求精确——I²C 是**同步**总线，从机完全跟随主机时钟，
 * 只要不超过器件支持的上限（本项目 100kHz）就一定能通。
 * 72MHz 下次循环约 6 个周期，360 周期 ≈ 5µs。
 */
static void i2c_delay(void)
{
    volatile uint32_t n = (SystemCoreClock / 1000000u) * I2C_DELAY_US / 4u;

    while (n-- > 0u)
    {
        __NOP();
    }
}

/* ==========================================================================
 * 总线原语
 * ========================================================================== */

void i2c_bus_start(void)
{
    sda_release();
    i2c_delay();
    (void)scl_release_and_wait();
    i2c_delay();
    sda_low();          /* SCL 为高时 SDA 下降沿 = 起始条件 */
    i2c_delay();
    scl_low();
    i2c_delay();
}

void i2c_bus_stop(void)
{
    sda_low();
    i2c_delay();
    (void)scl_release_and_wait();
    i2c_delay();
    sda_release();      /* SCL 为高时 SDA 上升沿 = 停止条件 */
    i2c_delay();
}

bool i2c_bus_write(uint8_t byte)
{
    uint8_t i;

    for (i = 0u; i < 8u; i++)
    {
        /* I²C 规定：数据只能在 SCL 为低的期间变化 */
        sda_write((byte & 0x80u) ? 1u : 0u);
        byte = (uint8_t)(byte << 1);
        i2c_delay();

        /* ★ 快速失败：SCL 拉不高说明总线有问题（多半是没上拉电阻），
           立刻放弃，不要傻等完剩下的位。 */
        if (!scl_release_and_wait())
        {
            s_nack_count++;
            sda_release();
            return false;
        }

        i2c_delay();
        scl_low();
        i2c_delay();
    }

    /* 第 9 个时钟：主机释放 SDA，由从机拉低表示应答 */
    sda_release();
    i2c_delay();
    if (!scl_release_and_wait())
    {
        s_nack_count++;
        return false;
    }
    i2c_delay();

    {
        uint8_t ack = (sda_read() == 0u) ? 1u : 0u;   /* 低电平 = ACK */

        scl_low();
        i2c_delay();

        if (ack == 0u)
        {
            /* 无应答。最常见的原因是器件地址写错、器件没供电、
               或者接线松了——这三条按顺序查。 */
            s_nack_count++;
        }
        return (ack != 0u);
    }
}

uint8_t i2c_bus_read(bool ack)
{
    uint8_t i;
    uint8_t byte = 0u;

    sda_release();      /* 主机交出 SDA 控制权，由从机驱动 */

    for (i = 0u; i < 8u; i++)
    {
        i2c_delay();
        if (!scl_release_and_wait())
        {
            s_nack_count++;
            sda_release();
            return 0u;      /* 总线异常，返回的字节无意义，由调用者按失败处理 */
        }
        i2c_delay();
        byte = (uint8_t)(byte << 1);
        if (sda_read() != 0u)
        {
            byte |= 0x01u;
        }
        scl_low();
        i2c_delay();
    }

    /* 第 9 个时钟：主机发 ACK（拉低）表示还要继续读，
       发 NACK（释放）表示这是最后一个字节。 */
    sda_write(ack ? 0u : 1u);
    i2c_delay();
    (void)scl_release_and_wait();
    i2c_delay();
    scl_low();
    i2c_delay();
    sda_release();

    return byte;
}

/* ==========================================================================
 * 总线恢复
 * ========================================================================== */

/**
 * @brief 从"总线卡死"状态里恢复。
 *
 * 什么情况会卡死：主机在从机正在输出某一位的**中途**复位/掉电，从机会
 * 一直等下一个时钟，把 SDA 拉住不放。此时下一次通信的起始条件根本发不出去。
 *
 * 标准解法：主机手动发最多 9 个 SCL 脉冲，让从机把剩下没读完的位吐完，
 * 它自己就会释放 SDA；然后补一个停止条件，把所有器件复位到空闲态。
 */
static void i2c_bus_recover(void)
{
    uint8_t i;

    sda_release();
    i2c_delay();

    for (i = 0u; i < 9u; i++)
    {
        if (sda_read() != 0u)
        {
            break;              /* SDA 已经被放开，说明从机吐完了 */
        }
        scl_low();
        i2c_delay();
        (void)scl_release_and_wait();
        i2c_delay();
    }

    i2c_bus_stop();
}

/* ==========================================================================
 * 互斥锁
 * ========================================================================== */

void i2c_init(void)
{
    const osMutexAttr_t attr = {
        .name      = "i2cBus",
        /* ★ 这两个属性是面试必问点，见 app_i2c.h 里的详细说明：
           PrioInherit 防优先级反转，Recursive 支持事务内嵌套加锁 */
        .attr_bits = osMutexPrioInherit | osMutexRecursive,
        .cb_mem    = NULL,
        .cb_size   = 0u,
    };

    s_i2c_mutex = osMutexNew(&attr);
    if (s_i2c_mutex == NULL)
    {
        Error_Handler();
    }

    /* 上电时两根线都放开，然后做一次总线恢复，
       把可能处于"半途"状态的从机拉回空闲。
       两个器件都有上拉电阻时，这一步是安全且必要的。 */
    sda_release();
    scl_release();
    i2c_bus_recover();
}

bool i2c_transaction(i2c_op_fn fn, void *ctx, uint32_t timeout_ms)
{
    bool result;

    if (s_i2c_mutex == NULL || fn == NULL)
    {
        return false;       /* i2c_init 没调用、或传了空回调，属于编程错误 */
    }

    if (osMutexAcquire(s_i2c_mutex, timeout_ms) != osOK)
    {
        /* 超时。宁可不读这一次，也不阻塞任务——
           数据丢一帧，上位机会算进丢包率；任务阻塞则会连锁拖垮整条链路。 */
        s_timeout_count++;
        return false;
    }

    result = fn(ctx);       /* ← 临界区：完整的一次 I²C 事务 */

    (void)osMutexRelease(s_i2c_mutex);
    return result;
}

/* ==========================================================================
 * 诊断
 * ========================================================================== */

uint32_t i2c_get_timeout_count(void)
{
    return s_timeout_count;
}

uint32_t i2c_get_nack_count(void)
{
    return s_nack_count;
}
