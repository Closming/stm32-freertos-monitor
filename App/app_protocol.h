/**
  ******************************************************************************
  * @file    app_protocol.h
  * @brief   串口上报协议：CRC16-Modbus + 帧打包。
  *
  * 与上位机 Host/frame_codec.py 是同一份规范的两种实现，改任何一边
  * 都必须同步改另一边，否则 CRC 对不上、整个上位机收不到数据。
  * Host/test_codec.py 里有一条测试会直接解析本文件里的 CRC 表做比对，
  * 表一旦漂移就会失败。
  ******************************************************************************
  */

#ifndef __APP_PROTOCOL_H
#define __APP_PROTOCOL_H

#include <stdint.h>
#include "app_config.h"

/**
 * @brief 一次采样凑齐的四路数据（温湿度已 ×100 存成整数）
 */
typedef struct
{
    int16_t  temp_x100;   /**< 温度 ×100，-4000 表示 -40.00℃ */
    int16_t  humi_x100;   /**< 湿度 ×100 */
    uint16_t light;       /**< 光照 ADC 原始值 0~4095 */
    uint16_t pot;         /**< 电位器 ADC 原始值 0~4095 */
} sensor_sample_t;

/**
 * @brief 算 CRC16-Modbus（查表法）
 *
 * 多项式 0xA001（0x8005 的反射形式），初值 0xFFFF。
 * 对 ASCII "123456789" 的结果应为 0x4B37 —— 这是 CRC-16/MODBUS 的
 * 标准校验值，上位机测试里也验了同一个向量。
 *
 * @param data 数据首地址
 * @param len  字节数
 * @return 16 位校验值
 */
uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

/**
 * @brief 按协议打包一帧
 *
 * 帧长恒为 FRAME_TOTAL_LEN（14）字节。
 * 调用者负责保证 out 至少有 FRAME_TOTAL_LEN 字节的空间。
 *
 * @param out 输出缓冲区
 * @param seq 帧序号，0~255 循环，上位机靠它算丢包率
 * @param s   四路采样数据
 * @return 实际写入的字节数（恒为 14）
 */
uint8_t frame_build(uint8_t *out, uint8_t seq, const sensor_sample_t *s);

#endif /* __APP_PROTOCOL_H */
