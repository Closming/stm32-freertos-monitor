/**
  ******************************************************************************
  * @file    app_uart.c
  * @brief   串口实现：printf 重定向、DMA 发帧、任务安全日志。
  ******************************************************************************
  */

#include "app_uart.h"
#include "cmsis_os2.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* CubeMX 在 main.c 里定义的句柄。这里自己 extern 一份，
   这样本文件在 CubeMX 还没生成 usart.c 时也能编译检查。 */
extern UART_HandleTypeDef huart1;

/* ==========================================================================
 * 模块内部状态（必须先声明再用 —— C 里这是硬性要求）
 * ========================================================================== */
static osMutexId_t     s_uart_mutex = NULL;   /**< 串行化所有 USART1 发送 */
static osSemaphoreId_t s_tx_done    = NULL;   /**< DMA 发送完成信号 */

/* ==========================================================================
 * 初始化
 * ========================================================================== */

void uart_init(void)
{
    const osMutexAttr_t mutex_attr = {
        .name      = "uartTx",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0u,
    };

    /* 二值信号量：初值 0，DMA 发完了才由中断释放一个 */
    s_uart_mutex = osMutexNew(&mutex_attr);
    s_tx_done    = osSemaphoreNew(1u, 0u, NULL);

    if (s_uart_mutex == NULL || s_tx_done == NULL)
    {
        Error_Handler();
    }
}

/* ==========================================================================
 * DMA 完成回调
 *
 * ★ 这里跑在**中断上下文**里，能调用的 API 有严格限制。
 *   实测确认过 ST 的实现：osSemaphoreRelease() 内部会检查 IS_IRQ()，
 *   中断里自动改走 xSemaphoreGiveFromISR + portYIELD_FROM_ISR，
 *   所以它是 ISR 安全的。
 *
 *   ⚠️ 但**不能**在这里拿互斥锁（osMutexAcquire）。FreeRTOS 明确禁止
 *   在中断里获取互斥锁。所以这里只做"释放信号量"这一件事。
 * ========================================================================== */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        (void)osSemaphoreRelease(s_tx_done);
    }
}

/**
 * @brief 发送出错时也要放行，否则等待方会一直等到超时。
 *
 * 典型错误：DMA 传输错误、帧错误。不处理的话 HAL 不会再回调 TxCplt，
 * uart_send_frame 里的等待就会白等到超时，白白浪费 100ms。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        (void)osSemaphoreRelease(s_tx_done);
    }
}

/* ==========================================================================
 * 内部：加锁的阻塞发送（用于日志）
 * ========================================================================== */

static void uart_write_locked(const uint8_t *buf, uint16_t len)
{
    /* 调度器还没跑起来时不能拿互斥锁（FreeRTOS 不允许），
       此时直接发。初始化阶段的日志就属于这种情况。 */
    if (s_uart_mutex != NULL && osKernelGetState() == osKernelRunning)
    {
        if (osMutexAcquire(s_uart_mutex, UART_TX_TIMEOUT_MS) != osOK)
        {
            return;     /* 拿不到锁就放弃这条日志，日志不值得阻塞任务 */
        }
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, UART_TX_TIMEOUT_MS);
        (void)osMutexRelease(s_uart_mutex);
    }
    else
    {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, UART_TX_TIMEOUT_MS);
    }
}

/* ==========================================================================
 * printf 重定向
 *
 * Keil 工程里勾了 Use MicroLIB，所以只需要实现 fputc 就能让 printf 走串口。
 * 没勾 MicroLIB 的话还要额外处理 __use_no_semihosting 和 _sys_exit 之类，
 * 麻烦得多 —— 这也是工程里保持 MicroLIB 开启的原因。
 * ========================================================================== */

int fputc(int ch, FILE *f)
{
    uint8_t byte = (uint8_t)ch;

    (void)f;
    uart_write_locked(&byte, 1u);

    return ch;
}

/* ==========================================================================
 * 任务安全日志
 * ========================================================================== */

void uart_log(const char *fmt, ...)
{
    char    buf[UART_LOG_BUF_SIZE];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0)
    {
        return;
    }
    if (n > (int)sizeof(buf) - 1)
    {
        n = (int)sizeof(buf) - 1;   /* vsnprintf 会截断，这里只是同步长度 */
    }

    uart_write_locked((const uint8_t *)buf, (uint16_t)n);
}

/* ==========================================================================
 * 帧发送（DMA）
 * ========================================================================== */

bool uart_send_frame(const uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef st;

    if (s_uart_mutex == NULL || buf == NULL || len == 0u)
    {
        return false;
    }

    if (osMutexAcquire(s_uart_mutex, UART_TX_TIMEOUT_MS) != osOK)
    {
        return false;
    }

    /* ★ 先清掉可能残留的信号量计数。
       上一次发送的信号量如果没被取走，这里不排空的话，
       下面的 Acquire 会立刻返回，我们就会在 DMA 还没发完时
       误以为已发送完成。 */
    (void)osSemaphoreAcquire(s_tx_done, 0u);

    st = HAL_UART_Transmit_DMA(&huart1, (uint8_t *)buf, len);

    if (st == HAL_OK)
    {
        /* 等中断里的 osSemaphoreRelease。等待期间不占 CPU 忙等，
           这正是用 DMA 而非轮询发送的意义。 */
        (void)osSemaphoreAcquire(s_tx_done, UART_TX_TIMEOUT_MS);
    }

    (void)osMutexRelease(s_uart_mutex);

    return (st == HAL_OK);
}
