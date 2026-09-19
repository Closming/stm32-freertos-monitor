/**
  ******************************************************************************
  * @file    app_adc.c
  * @brief   ADC + DMA 采集与滑动平均滤波。
  ******************************************************************************
  */

#include "app_adc.h"

/* CubeMX 在 adc.c 里定义的句柄。自己 extern 一份，
   这样 CubeMX 还没配 ADC 时本文件也能被编译检查。 */
extern ADC_HandleTypeDef hadc1;

/* ==========================================================================
 * 模块内部状态（先声明后用）
 * ========================================================================== */

/** DMA 循环搬运的落点。DMA 会一直往里写，我们只读，不需要 volatile 数组
    ——但单个元素是 16 位，Cortex-M3 上 16 位对齐读是原子的，
    所以即使读的瞬间 DMA 正在更新，也不会读到"半个数"。 */
static uint16_t s_dma_buf[ADC_DMA_LEN];

/** 滑动平均窗口，每个通道一个环形缓冲 */
static uint16_t s_window[ADC_DMA_LEN][ADC_FILTER_WIN];

/** 环形缓冲的写指针，两个通道共用（每次调用推进一格） */
static uint8_t s_win_idx = 0u;

/** 窗口是否已填满。没填满时不能直接除以窗口大小，否则开头几个值会被拉低 */
static uint8_t s_win_filled = 0u;

/* ==========================================================================
 * 初始化
 * ========================================================================== */

void adc_init(void)
{
    uint8_t ch;
    uint8_t i;

    for (ch = 0u; ch < ADC_DMA_LEN; ch++)
    {
        s_dma_buf[ch] = 0u;
        for (i = 0u; i < ADC_FILTER_WIN; i++)
        {
            s_window[ch][i] = 0u;
        }
    }
    s_win_idx    = 0u;
    s_win_filled = 0u;

    /* ★ STM32F1 的 ADC 上电后必须校准一次（校准内部电容组），
       否则读数存在固定偏差。CubeMX 不会自动加这一步，手册里也常漏。
       校准必须在 ADC 使能之前、且只能在 ADC 关闭状态下做。 */
    if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    /* HAL_ADC_Start_DMA 的形参类型是 uint32_t*，但实际搬运宽度由
       CubeMX 里 DMA 的配置决定（这里是半字 16 位）。这个转型是 HAL
       的历史遗留，看起来别扭但是标准用法。 */
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_dma_buf, ADC_DMA_LEN) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ==========================================================================
 * 读取
 * ========================================================================== */

void adc_get_raw(adc_sample_t *out)
{
    if (out == NULL)
    {
        return;
    }

    out->light = s_dma_buf[ADC_CH_LIGHT];
    out->pot   = s_dma_buf[ADC_CH_POT];
}

void adc_get_filtered(adc_sample_t *out)
{
    uint16_t result[ADC_DMA_LEN];
    uint8_t  ch;
    uint8_t  i;

    if (out == NULL)
    {
        return;
    }

    /* 1) 把当前原始值推进窗口 */
    for (ch = 0u; ch < ADC_DMA_LEN; ch++)
    {
        s_window[ch][s_win_idx] = s_dma_buf[ch];
    }
    s_win_idx = (uint8_t)((s_win_idx + 1u) % ADC_FILTER_WIN);

    /* 2) 窗口填满后，除数固定用 ADC_FILTER_WIN；
          没填满时只用已经攒到的点数，避免开头把均值拉向 0。 */
    if (s_win_filled < ADC_FILTER_WIN)
    {
        s_win_filled++;
    }

    for (ch = 0u; ch < ADC_DMA_LEN; ch++)
    {
        uint32_t sum = 0u;

        for (i = 0u; i < s_win_filled; i++)
        {
            sum += s_window[ch][i];
        }
        result[ch] = (uint16_t)(sum / s_win_filled);
    }

    out->light = result[ADC_CH_LIGHT];
    out->pot   = result[ADC_CH_POT];
}
