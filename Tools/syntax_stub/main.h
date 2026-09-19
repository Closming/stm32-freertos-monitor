/**
  ******************************************************************************
  * @file    main.h  （语法检查专用的桩文件，不是给单片机用的）
  *
  * ★ 这个文件**不参与固件编译**，只被 Tools/check_app.sh 使用。
  *
  * 为什么需要它：App/ 下的代码引用了 CubeMX 生成的引脚宏（I2C_SCL_Pin 等），
  * 那些宏在真机上由 CubeMX 写进工程根目录的 Inc/main.h。但 App/ 的代码
  * 往往在 CubeMX 配置完成之前就写好了，此时没有真正的 main.h，
  * 编译器会报一堆"标识符未定义"——掩盖掉真正的语法错误。
  *
  * 这里把那些宏按 .ioc 里约定的引脚补上，让 App 目录下的源文件在任何
  * 时候都能被编译检查。
  *
  * ⚠️ 引脚一旦在 CubeMX 里改动，记得同步改这里，否则检查结果不可信。
  ******************************************************************************
  */

#ifndef __MAIN_H
#define __MAIN_H

#include "stm32f1xx_hal.h"

/* 软件 I²C —— PB6 / PB7 */
#define I2C_SCL_Pin        GPIO_PIN_6
#define I2C_SCL_GPIO_Port  GPIOB
#define I2C_SDA_Pin        GPIO_PIN_7
#define I2C_SDA_GPIO_Port  GPIOB

/* 按键 —— PA3（刻意避开板载按键所在的 PA0） */
#define KEY_Pin            GPIO_PIN_3
#define KEY_GPIO_Port      GPIOA

/* 蜂鸣器 —— PB0 */
#define BEEP_Pin           GPIO_PIN_0
#define BEEP_GPIO_Port     GPIOB

/* 板载 LED —— PC13，低电平点亮 */
#define LED_Pin            GPIO_PIN_13
#define LED_GPIO_Port      GPIOC

/* CubeMX 生成在 main.c 里的错误处理函数 */
void Error_Handler(void);

#endif /* __MAIN_H */
