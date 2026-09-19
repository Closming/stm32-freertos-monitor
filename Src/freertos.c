/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                             www.st.com/SLA0044
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_tasks.h"     

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
   
/* USER CODE END FunctionPrototypes */


void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
       
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* ★ 这里原本是 CubeMX 生成的 defaultTask，已删除，原因见下方说明 */

  /* USER CODE BEGIN RTOS_THREADS */

  /* 创建队列 / 事件组 / 互斥锁，并创建六个任务（实现见 App/app_tasks.c） */
  app_init();

  /* ★ 为什么删掉了 CubeMX 生成的 defaultTask：
   *
   *   CubeMX 是按 CMSIS-RTOS **V1** 规则生成代码的，那里的宏是
   *       osThreadDef(name, thread, priority, instances, stacksz)   <- 5 个参数
   *   而本项目用的是 **V2**，V2 兼容层里这个宏是
   *       osThreadDef(name, priority, instances, stacksz)           <- 4 个参数
   *   （V2 把"入口函数"延后到 osThreadNew 时才传，不再放进定义里）
   *
   *   所以 CubeMX 生成的那一行直接编译不过。而它本来也只是个
   *   `for(;;) osDelay(1);` 的空循环，六个真正的任务全部由 app_init()
   *   创建 —— 删掉它既解决了编译错误，也省下一个任务槽和 512 字节栈。
   *
   *   ⚠️ 将来若在 CubeMX 里重新 GENERATE CODE，它会被重新生成出来，
   *      需要再删一次。详见 README「已知局限」。 */

  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
     
/* USER CODE END Application */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
