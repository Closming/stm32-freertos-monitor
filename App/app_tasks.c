/**
  ******************************************************************************
  * @file    app_tasks.c
  * @brief   六个任务的定义、任务间通信、以及看门狗与心跳监控。
  ******************************************************************************
  */

#include "app_tasks.h"
#include "app_adc.h"
#include "app_i2c.h"
#include "app_protocol.h"
#include "app_sht30.h"
#include "app_ssd1306.h"
#include "app_uart.h"
#include "app_font.h"
#include "cmsis_os2.h"

/* CubeMX 生成的句柄。自己 extern 一份，这样 CubeMX 还没配 IWDG 时
   本文件也能被 Tools/check_app.sh 编译检查。 */
extern IWDG_HandleTypeDef hiwdg;

/* ==========================================================================
 * 优先级
 *
 * ★ CMSIS-RTOS2 的优先级是 osPriority_t **枚举**，不是数字。映射到
 *   FreeRTOS 后（configMAX_PRIORITIES = 7）实际只有 5 个可用档位，
 *   所以下面六个任务里有四个是共享档位的。
 *
 *   排序依据两个因素：实时性要求、以及**持总线锁的时长**。
 *
 *     Acquire  最高 —— 50ms 硬周期，晚了滑动平均窗口就会抖
 *     Sensor   次高 —— 要抢总线锁读 SHT30，必须排在 Display 之上
 *     Process  中间 —— 常规周期任务
 *     Monitor  中间 —— 喂狗 + 心跳，不能比它监控的任务还低
 *     Comm     偏低 —— UART DMA 发送，等得起
 *     Display  最低 —— ★ 持锁最久（刷一屏约 150ms，拆成 4 段仍是最长的），
 *                      给高了会把传感器读数堵在后面
 *
 *   ⚠️ 方案文档里写的"采集 4 / 监控 2 / 显示 1"是 FreeRTOS 的**数字**
 *      优先级口径，在 CMSIS-RTOS2 下用不了。相对顺序是一致的。
 * ========================================================================== */
#define PRIO_ACQUIRE    osPriorityRealtime
#define PRIO_SENSOR     osPriorityHigh
#define PRIO_PROCESS    osPriorityAboveNormal
#define PRIO_MONITOR    osPriorityAboveNormal
#define PRIO_COMM       osPriorityNormal
#define PRIO_DISPLAY    osPriorityBelowNormal

/** 按键消抖窗口（ms）。机械按键按下时电平会抖十几毫秒，不消抖会一次按下
 *  触发好几次中断。这里在 ISR 里用时间戳做最简消抖 —— 比在 ISR 里
 *  延时等待正确得多（中断里绝不能延时）。 */
#define KEY_DEBOUNCE_MS 50u

/* ==========================================================================
 * 模块内部状态（必须先声明后用）
 * ========================================================================== */

/** 心跳计数器的下标。★ 数组长度靠 TASK_ID_COUNT 兜底，加任务时别忘了改 */
typedef enum
{
    TASK_ID_ACQUIRE = 0,
    TASK_ID_SENSOR,
    TASK_ID_PROCESS,
    TASK_ID_DISPLAY,
    TASK_ID_COMM,
    TASK_ID_MONITOR,
    TASK_ID_COUNT
} task_id_t;

/** SHT30 读数的消息体：数据 + 有效性标志。
 *
 *  ★ 必须是有名字的类型，不能图省事写成匿名结构体 ——
 *    C 里两处分别写的 `struct { ... }` 是**两个不同类型**，
 *    虽然 sizeof 恰好相同、运行时也能跑，但语义上是错的，
 *    将来往里加字段时两边不同步就会踩坑。
 *
 *  为什么把 valid 和数据打包在一起发：分成两条消息的话，消费者可能
 *  读到"新数据配旧的 valid"，出现撕裂状态。打包就没有这个窗口。 */
typedef struct
{
    sht30_data_t data;
    bool         valid;
} sht30_msg_t;

/** 各任务的心跳计数。任务自己加，TaskMonitor 定期读着比对。
 *  用 volatile 是因为它会被别的任务异步读到。 */
static volatile uint32_t s_heartbeat[TASK_ID_COUNT];

static const char *const s_task_names[TASK_ID_COUNT] =
{
    "Acquire", "Sensor", "Process", "Display", "Comm", "Monitor"
};

/* 任务句柄 */
static osThreadId_t s_acquire_thread;
static osThreadId_t s_sensor_thread;
static osThreadId_t s_process_thread;
static osThreadId_t s_display_thread;
static osThreadId_t s_comm_thread;
static osThreadId_t s_monitor_thread;

/* 队列 */
static osMessageQueueId_t s_adc_queue;      /**< TaskAcquire → TaskProcess */
static osMessageQueueId_t s_sht30_queue;    /**< TaskSensor  → TaskProcess */
static osMessageQueueId_t s_display_queue;  /**< TaskProcess → TaskDisplay */
static osMessageQueueId_t s_report_queue;   /**< TaskProcess → TaskComm */

/* 事件组 */
static osEventFlagsId_t s_events;

/* TaskMonitor 的任务通知标志位 */
#define FLAG_KEY_PRESSED   (1UL << 0)

/* 消音状态与按键计数 */
static volatile uint8_t  s_muted      = 0u;
static volatile uint32_t s_key_count  = 0u;
static uint32_t          s_last_key_tick = 0u;

/* 板载 LED 心跳分频计数（采集任务每 20 轮 = 1s 翻转一次） */
static uint8_t s_led_divider = 0u;

/* 心跳停滞检测 */
static uint32_t s_last_heartbeat[TASK_ID_COUNT];
static uint8_t  s_stall_count[TASK_ID_COUNT];

/* ==========================================================================
 * 内部辅助
 * ========================================================================== */

/** 从队列里取出**最新**的一份 ADC 数据，把积压的旧数据全部倒掉。
 *
 *  为什么要倒掉：采集是 50ms 一次，而处理和上报是约 1s 一次。
 *  积压的数据只会让人看到"过去的平均值"而不是"现在是多少"。
 *  队列在这里的作用不是缓存，而是**解耦生产者和消费者的节奏** ——
 *  有了队列，TaskAcquire 永远不阻塞，不用管消费者忙不忙。
 */
static bool drain_latest_adc(adc_sample_t *out)
{
    adc_sample_t tmp;
    bool         got = false;

    while (osMessageQueueGet(s_adc_queue, &tmp, NULL, 0u) == osOK)
    {
        *out = tmp;
        got  = true;
    }
    return got;
}

/** 画温度。单独抽出来是为了处理 -0.xx℃ 这个别扭的边界情况：
 *  整数除法 -34/100 得 0（不是 -0），而 -34%100 得 -34，
 *  拼出来会变成 "0.-34"。所以负数和整数部分要分开处理。 */
static void draw_temperature(uint8_t page, int16_t temp_x100)
{
    int16_t whole = (int16_t)(temp_x100 / 100);
    int16_t frac  = (int16_t)(temp_x100 % 100);

    if (frac < 0)
    {
        frac = (int16_t)(-frac);
    }

    if (temp_x100 < 0 && whole == 0)
    {
        ssd1306_printf(page, "TEMP -0.%02d" FONT_DEG "C", (int)frac);
    }
    else
    {
        ssd1306_printf(page, "TEMP %d.%02d" FONT_DEG "C", (int)whole, (int)frac);
    }
}

/** 把一帧采样画到显存（不负责刷新，由调用者决定何时 flush） */
static void render_screen(const app_sample_t *s)
{
    ssd1306_clear();

    ssd1306_draw_text_centered(0u, "ENV MONITOR");

    if ((s->alarm & ALARM_SENSOR_ERR) != 0u)
    {
        ssd1306_draw_text(2u, 0u, "TEMP --.--");
        ssd1306_draw_text(3u, 0u, "HUMI --.--");
    }
    else
    {
        draw_temperature(2u, s->temp_x100);
        ssd1306_printf(3u, "HUMI %d.%02d%%",
                       (int)(s->humi_x100 / 100),
                       (int)(s->humi_x100 % 100));
    }

    ssd1306_printf(5u, "LGT %4u", (unsigned)s->light);
    ssd1306_printf(6u, "POT %4u", (unsigned)s->pot);

    if (s->alarm != 0u)
    {
        if (s_muted != 0u)
        {
            ssd1306_draw_text(7u, 0u, "ALARM (MUTE)");
        }
        else
        {
            ssd1306_draw_text(7u, 0u, "!! ALARM !!");
        }
    }
    else
    {
        ssd1306_draw_text(7u, 0u, "STATUS OK");
    }
}

/* ==========================================================================
 * 六个任务
 * ========================================================================== */

/** 采集任务：50ms 周期读滤波后的 ADC 值，发队列并置事件位 */
static void task_acquire(void *argument)
{
    adc_sample_t adc;

    (void)argument;

    for (;;)
    {
        adc_get_filtered(&adc);

        /* 超时 0：队列满就直接丢这一份，绝不阻塞采集 ——
           采集周期是整条链路的节拍，它一旦抖动，后面全是连锁反应。 */
        if (osMessageQueuePut(s_adc_queue, &adc, 0u, 0u) != osOK)
        {
            adc_sample_t discard;
            (void)osMessageQueueGet(s_adc_queue, &discard, NULL, 0u);
            (void)osMessageQueuePut(s_adc_queue, &adc, 0u, 0u);
        }

        (void)osEventFlagsSet(s_events, EVT_FLAG_ADC_READY);

        /* ★ 板载 LED 每秒翻转一次，作为"调度器正在跑"的心跳指示。
         *
         *   为什么需要它：调度器一旦启动，main() 里那个 while(1) 点灯
         *   循环就永远不可达了，板子上再也没有"我还活着"的视觉反馈。
         *   调试时（尤其没串口的情况下）有个灯在闪，能立刻区分
         *   "程序跑起来了但功能有问题" 和 "程序根本没跑起来"。 */
        if (++s_led_divider >= (1000u / TASK_ACQUIRE_PERIOD_MS))
        {
            s_led_divider = 0u;
            LED_TOGGLE();
        }

        s_heartbeat[TASK_ID_ACQUIRE]++;
        osDelay(TASK_ACQUIRE_PERIOD_MS);
    }
}

/** 传感器任务：1s 周期读 SHT30。读一次要两次 I²C 事务 + 20ms 等待 */
static void task_sensor(void *argument)
{
    sht30_data_t  data;
    bool          valid;

    (void)argument;

    for (;;)
    {
        valid = sht30_read(&data);

        {
            sht30_msg_t msg;

            msg.data  = data;
            msg.valid = valid;

            if (osMessageQueuePut(s_sht30_queue, &msg, 0u, 0u) != osOK)
            {
                /* 队列满说明处理任务落后了，丢最旧的一份 */
                (void)osMessageQueueGet(s_sht30_queue, &msg, NULL, 0u);
                (void)osMessageQueuePut(s_sht30_queue, &msg, 0u, 0u);
            }
        }

        (void)osEventFlagsSet(s_events, EVT_FLAG_SHT30_READY);

        s_heartbeat[TASK_ID_SENSOR]++;
        osDelay(TASK_SENSOR_PERIOD_MS);
    }
}

/** 处理任务：等两路数据都就绪 → 越限判定 → 分发到显示队列和上报队列 */
static void task_process(void *argument)
{
    (void)argument;

    for (;;)
    {
        app_sample_t out;
        adc_sample_t adc;
        sht30_data_t sht;
        bool         sht_valid = false;
        uint32_t     flags;

        /* ★ 事件组的核心用途：**多源同步**。
           osFlagsWaitAll 要求两位都置位才返回，返回时自动清掉这两位 ——
           天然的"到齐一次、处理一次"语义，不需要额外的握手。
           因为 ADC 位每 50ms 就置一次、SHT30 位每 1s 置一次，
           实际效果是本任务**约每秒被唤醒一次**。 */
        flags = osEventFlagsWait(s_events, EVT_FLAG_DATA_READY,
                                 osFlagsWaitAll, osWaitForever);

        if ((flags & EVT_FLAG_DATA_READY) != EVT_FLAG_DATA_READY)
        {
            continue;   /* 被 clear 或出错，重来 */
        }

        if (!drain_latest_adc(&adc))
        {
            continue;
        }

        {
            sht30_msg_t msg;

            if (osMessageQueueGet(s_sht30_queue, &msg, NULL, 0u) == osOK)
            {
                sht       = msg.data;
                sht_valid = msg.valid;
            }
        }

        /* ---- 组装 ---- */
        out.light  = adc.light;
        out.pot    = adc.pot;
        out.alarm  = 0u;

        if (sht_valid)
        {
            out.temp_x100 = sht.temp_x100;
            out.humi_x100 = sht.humi_x100;

            if (sht.temp_x100 > TH_TEMP_HIGH_X100) { out.alarm |= ALARM_TEMP_HIGH; }
            if (sht.temp_x100 < TH_TEMP_LOW_X100)  { out.alarm |= ALARM_TEMP_LOW; }
            if (sht.humi_x100 > TH_HUMI_HIGH_X100) { out.alarm |= ALARM_HUMI_HIGH; }
            if (sht.humi_x100 < TH_HUMI_LOW_X100)  { out.alarm |= ALARM_HUMI_LOW; }
        }
        else
        {
            out.temp_x100 = 0;
            out.humi_x100 = 0;
            out.alarm    |= ALARM_SENSOR_ERR;
        }

        if (adc.light > TH_LIGHT_HIGH) { out.alarm |= ALARM_LIGHT_HIGH; }
        if (adc.light < TH_LIGHT_LOW)  { out.alarm |= ALARM_LIGHT_LOW; }

        /* ---- 广播报警状态（TaskMonitor 读它来决定要不要响蜂鸣器）---- */
        if (out.alarm != 0u)
        {
            (void)osEventFlagsSet(s_events, EVT_FLAG_ALARM);
        }
        else
        {
            (void)osEventFlagsClear(s_events, EVT_FLAG_ALARM);
        }

        /* ---- 分发 ----
           ★ 显示和上报需要**同一份**数据，所以要投递到两条队列。
              队列是点对点的：同一条队列里的每一条消息只会被一个消费者取走，
              两个消费者共用一个队列会各拿到一半，那是错的。 */
        (void)osMessageQueuePut(s_display_queue, &out, 0u, 0u);
        (void)osMessageQueuePut(s_report_queue, &out, 0u, 0u);

        s_heartbeat[TASK_ID_PROCESS]++;
    }
}

/** 显示任务：刷 OLED。持总线锁最久，所以优先级最低 */
static void task_display(void *argument)
{
    app_sample_t s;
    bool         inited = false;

    (void)argument;

    for (;;)
    {
        /* ★ OLED 初始化必须在这里做，不能在 app_init() 里：
           ssd1306_init() 要发 I²C 命令，会走 i2c_transaction() 去拿互斥锁，
           而 **FreeRTOS 不允许在调度器启动之前获取互斥锁**。
           放进任务的第一轮循环，就天然满足了"调度器已启动"这个前提。 */
        if (!inited)
        {
            ssd1306_init();
            inited = true;
            uart_log("[DISP] OLED init done\r\n");
        }

        if (osMessageQueueGet(s_display_queue, &s, NULL, 0u) == osOK)
        {
            render_screen(&s);

            /* ★ 刷屏失败不能静默。原来这里是 `(void)ssd1306_flush();`，
               返回值被丢掉、s_flush_error_count 又没人读 —— 屏不亮的时候
               串口上一条线索都没有，只能在硬件上瞎猜。
               失败率不用很精确，能看出"在刷但刷不过去"就够了。 */
            if (!ssd1306_flush())
            {
                uart_log("[DISP] flush FAILED, total=%u\r\n",
                         (unsigned)ssd1306_get_flush_error_count());
            }
        }

        s_heartbeat[TASK_ID_DISPLAY]++;
        osDelay(TASK_DISPLAY_PERIOD_MS);
    }
}

/** 通信任务：阻塞等上报队列，打包后走 UART DMA */
static void task_comm(void *argument)
{
    /* ★ 用 static 而不是栈上数组：uart_send_frame() 内部靠 DMA 发送，
       虽然它在返回前会等发送完成，但把缓冲区放在静态区可以避免
       将来有人把发送改成异步时踩到"栈已经释放但 DMA 还在读"的坑。 */
    static uint8_t s_frame[FRAME_TOTAL_LEN];
    static uint8_t s_seq = 0u;

    app_sample_t s;

    (void)argument;

    for (;;)
    {
        if (osMessageQueueGet(s_report_queue, &s, NULL, osWaitForever) != osOK)
        {
            continue;
        }

        {
            sensor_sample_t ss;

            ss.temp_x100 = s.temp_x100;
            ss.humi_x100 = s.humi_x100;
            ss.light     = s.light;
            ss.pot       = s.pot;

            (void)frame_build(s_frame, s_seq, &ss);
        }

        s_seq++;    /* 自然回绕：uint8_t 从 255 加回 0，上位机按取模算丢包率 */

        (void)uart_send_frame(s_frame, FRAME_TOTAL_LEN);

        s_heartbeat[TASK_ID_COMM]++;
    }
}

/** 监控任务：喂狗、查心跳、响应按键、驱动蜂鸣器 */
static void task_monitor(void *argument)
{
    (void)argument;

    for (;;)
    {
        /* ★ 用 osThreadFlagsWait 同时实现"周期性"和"事件驱动"：
           超时设成任务周期，所以它每 500ms 醒一次；但按键中断置位
           FLAG_KEY_PRESSED 时它会**立刻**醒来，不用等满 500ms。
           比"osDelay + 轮询标志位"响应更快，代码还更短。

           ★ 这里用的就是**任务通知** —— osThreadFlagsSet 内部走的是
             xTaskNotify / xTaskNotifyFromISR，是 FreeRTOS 里开销最小的
             任务间通信方式（不需要预先创建任何对象）。 */
        {
            uint32_t flags = osThreadFlagsWait(FLAG_KEY_PRESSED,
                                               osFlagsWaitAny,
                                               TASK_MONITOR_PERIOD_MS);

            if ((flags & FLAG_KEY_PRESSED) != 0u)
            {
                s_key_count++;
                s_muted = (uint8_t)(s_muted == 0u ? 1u : 0u);
                uart_log("[KEY] count=%u mute=%u\r\n",
                         (unsigned)s_key_count, (unsigned)s_muted);
            }
        }

        /* ---- 喂狗 ---- */
        (void)HAL_IWDG_Refresh(&hiwdg);

        /* ---- 心跳检查 ----
           只检查**别的**任务。Monitor 自己正在跑，它活着这件事不需要证明。 */
        {
            uint8_t i;

            for (i = 0u; i < TASK_ID_COUNT; i++)
            {
                if (i == (uint8_t)TASK_ID_MONITOR)
                {
                    continue;
                }

                if (s_heartbeat[i] == s_last_heartbeat[i])
                {
                    s_stall_count[i]++;
                    if (s_stall_count[i] == HEARTBEAT_STALL_LIMIT)
                    {
                        uart_log("[WDT] task %s STALLED\r\n", s_task_names[i]);
                    }
                }
                else
                {
                    s_stall_count[i] = 0u;
                }

                s_last_heartbeat[i] = s_heartbeat[i];
            }
        }

        /* ---- 蜂鸣器 ----
           报警状态由 TaskProcess 写在事件组里，这里直接读，不用队列。
           消音只是关声音，**报警状态本身不变** —— 显示上仍然会显示 ALARM，
           这样"静音"和"解除报警"是两件事，语义清楚。 */
        {
            uint32_t evt = osEventFlagsGet(s_events);

            if (((evt & EVT_FLAG_ALARM) != 0u) && (s_muted == 0u))
            {
                BEEP_ON();
            }
            else
            {
                BEEP_OFF();
            }
        }

        s_heartbeat[TASK_ID_MONITOR]++;
    }
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */

void app_init(void)
{
    const osThreadAttr_t acquire_attr = {
        .name = "Acquire", .priority = PRIO_ACQUIRE,
        .stack_size = TASK_ACQUIRE_STACK_BYTES,
    };
    const osThreadAttr_t sensor_attr = {
        .name = "Sensor", .priority = PRIO_SENSOR,
        .stack_size = TASK_SENSOR_STACK_BYTES,
    };
    const osThreadAttr_t process_attr = {
        .name = "Process", .priority = PRIO_PROCESS,
        .stack_size = TASK_PROCESS_STACK_BYTES,
    };
    const osThreadAttr_t display_attr = {
        .name = "Display", .priority = PRIO_DISPLAY,
        .stack_size = TASK_DISPLAY_STACK_BYTES,
    };
    const osThreadAttr_t comm_attr = {
        .name = "Comm", .priority = PRIO_COMM,
        .stack_size = TASK_COMM_STACK_BYTES,
    };
    const osThreadAttr_t monitor_attr = {
        .name = "Monitor", .priority = PRIO_MONITOR,
        .stack_size = TASK_MONITOR_STACK_BYTES,
    };

    uint8_t i;

    for (i = 0u; i < TASK_ID_COUNT; i++)
    {
        s_heartbeat[i]      = 0u;
        s_last_heartbeat[i] = 0u;
        s_stall_count[i]    = 0u;
    }

    /* ---- 外设（除了 OLED，它要等调度器跑起来）---- */
    i2c_init();
    uart_init();
    adc_init();

    /* ---- 队列 ---- */
    s_adc_queue     = osMessageQueueNew(QUEUE_SAMPLE_DEPTH, sizeof(adc_sample_t), NULL);
    s_sht30_queue   = osMessageQueueNew(QUEUE_SENSOR_DEPTH, sizeof(sht30_msg_t), NULL);
    s_display_queue = osMessageQueueNew(QUEUE_DISPLAY_DEPTH, sizeof(app_sample_t), NULL);
    s_report_queue  = osMessageQueueNew(QUEUE_DISPLAY_DEPTH, sizeof(app_sample_t), NULL);

    /* ---- 事件组 ---- */
    s_events = osEventFlagsNew(NULL);

    if (s_adc_queue == NULL || s_sht30_queue == NULL || s_display_queue == NULL ||
        s_report_queue == NULL || s_events == NULL)
    {
        Error_Handler();
    }

    /* ---- 任务 ---- */
    s_acquire_thread = osThreadNew(task_acquire, NULL, &acquire_attr);
    s_sensor_thread  = osThreadNew(task_sensor,  NULL, &sensor_attr);
    s_process_thread = osThreadNew(task_process, NULL, &process_attr);
    s_display_thread = osThreadNew(task_display, NULL, &display_attr);
    s_comm_thread    = osThreadNew(task_comm,    NULL, &comm_attr);
    s_monitor_thread = osThreadNew(task_monitor, NULL, &monitor_attr);

    if (s_acquire_thread == NULL || s_sensor_thread == NULL ||
        s_process_thread == NULL || s_display_thread == NULL ||
        s_comm_thread == NULL || s_monitor_thread == NULL)
    {
        Error_Handler();
    }
}

/* ==========================================================================
 * 按键中断
 * ========================================================================== */

void app_key_isr(void)
{
    uint32_t now = HAL_GetTick();

    /* 最简消抖：两次触发间隔太近就忽略。
       在 ISR 里做时间戳判断，而不是延时等待电平稳定 ——
       中断里绝不能阻塞。 */
    if ((uint32_t)(now - s_last_key_tick) < KEY_DEBOUNCE_MS)
    {
        return;
    }
    s_last_key_tick = now;

    /* ★ 只发任务通知，不做任何业务逻辑，立刻返回。
       中断执行时间越短越好，这是实时系统的铁律。 */
    (void)osThreadFlagsSet(s_monitor_thread, FLAG_KEY_PRESSED);
}

/** HAL 的 EXTI 回调（弱函数，这里覆盖它）。CubeMX 生成的驱动会调到这里。
 *
 *  ⚠️ 注意参数判断：EXTI 回调是所有 EXTI 线共用的，板上还有别的中断源时
 *     必须按引脚号区分，否则按了别的键也会走到我们的逻辑里。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY_Pin)
    {
        app_key_isr();
    }
}

/* ==========================================================================
 * 对外查询
 * ========================================================================== */

bool app_is_muted(void)
{
    return (s_muted != 0u);
}

uint32_t app_get_key_count(void)
{
    return s_key_count;
}
