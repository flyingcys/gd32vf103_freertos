/**
 * @file main.c
 * @author qiming.qiao
 * @brief gd32vf103 移植 FreeRTOS
 * @version 1.0
 * @date 2019-10-20
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#include <stdio.h>
#include "gd32vf103.h"
#include "SYSVIEW_Serial_Conf.h"
#include "SEGGER_RTT.h"
#include "FreeRTOS.h" 
#include "task.h"
#include "app_config.h"
/**
 * 调试探针（可选）：工程根下 src/debug_probe.h 存在时才编译进来。
 *
 * 缺省不定义 ⇒ 未放探针的工程行为完全不变。
 * 需要在调试器里断点观察共享目标时，把 debug_probe.{c,h} 放进 src/ 并在
 * 构建时加 -DGD32VF103_DEBUG_PROBE=1 即可。
 */
#if defined(GD32VF103_DEBUG_PROBE)
#include "debug_probe.h"
#endif


void uart_log_init(void)
{
    if(UARTLOG_PORT == USART0)
    {
        /* 初始化uart0 TX PA9 RX PA10 */
        rcu_periph_clock_enable(RCU_GPIOA);
        rcu_periph_clock_enable(RCU_USART0);
        gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_9);
        gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_10);
        usart_deinit(UARTLOG_PORT);
        usart_baudrate_set(UARTLOG_PORT, 115200U);
        usart_word_length_set(UARTLOG_PORT, USART_WL_8BIT);
        usart_stop_bit_set(UARTLOG_PORT, USART_STB_1BIT);
        usart_parity_config(UARTLOG_PORT, USART_PM_NONE);
        usart_hardware_flow_rts_config(UARTLOG_PORT, USART_RTS_DISABLE);
        usart_hardware_flow_cts_config(UARTLOG_PORT, USART_CTS_DISABLE);
        usart_receive_config(UARTLOG_PORT, USART_RECEIVE_ENABLE);
        usart_transmit_config(UARTLOG_PORT, USART_TRANSMIT_ENABLE);
        usart_enable(UARTLOG_PORT);
    }
}


void task1(void *p)
{
    for(;;)
    {
        gpio_bit_write(GPIOA, GPIO_PIN_7, (bit_status)(1-gpio_input_bit_get(GPIOA, GPIO_PIN_7)));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


void task2(void *p)
{
    char *taskStatus = (char *)pvPortMalloc( uxTaskGetNumberOfTasks() * sizeof( TaskStatus_t ) );
    for(;;)
    {
        vTaskList(taskStatus);
        printf("\nTaskName\tStatus\tPRI\tStack\tTaskNumber\n%s",taskStatus);
        printf("current tick is %ld\n",xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


int main(void)
{  
    eclic_priority_group_set(ECLIC_PRIGROUP_LEVEL4_PRIO0); //四位优先级组全配置为lvl
    eclic_global_interrupt_enable();                       //使能全局中断

    #if CONFIG_SYSTEMVIEW_EN
    SEGGER_SYSVIEW_Conf();
    printf("Segger Sysview Control Block Detection Address is 0x%lx\n", (uint32_t)&_SEGGER_RTT);
    vSYSVIEWUARTInit();
    #endif

    #if UARTLOGEN
    uart_log_init();
    #endif

    /* 初始化led PA7 */
    rcu_periph_clock_enable(RCU_GPIOA);
    gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_7);
    gpio_bit_reset(GPIOA, GPIO_PIN_7);

    xTaskCreate(task1,"task1",521,NULL,2,NULL);
    xTaskCreate(task2,"task2",521,NULL,2,NULL);

#if defined(GD32VF103_DEBUG_PROBE)
    /**
     * 调试探针：断点落在 dp_adder 内部（与芯片无关的共享目标）。
     *
     * ⚠️ 放在 `vTaskStartScheduler()` **之前**，不能放进任务里 —— 断点要在
     *    「continue 之后 20 秒内」命中，而任务里的调用周期由调度节拍决定
     *    （上游的 `vTaskDelay(500ms)`）。调度器若因缺节拍源没跑起来，探针
     *    永远不执行 ⇒ 断点永不命中，而症状会是「调试器没停」而不是「探针没跑」。
     * ⇒ 这里用一个带上下界的循环：既保证反复命中（断点可被多次命中），
     *    又保证没有调试器时会在有限时间内退出、继续启动调度器。
     */
    for (int i = 0; i < 8; i++) {
        dp_tick++;
        /**
         * ⚠️ **不能写 `if (dp_msg[0] == 0) for (;;);`** —— 那会死等一个字符串被改，
         *    放在 `main` 里会把启动卡死（旧夹具里那么写是因为它在任务循环里、
         *    且那场景本来就停在断点上）。这里只需**引用**它防止被 `--gc-sections` 丢掉。
         */
        if (dp_msg[0] == '\0') { dp_msg[0] = '\0'; }
        (void)dp_adder((int)dp_tick, 100);
        /**
         * ⚠️ **规模按断点场景的实际需求定，不是"感觉够大"**。
         *
         * `debug-breakpoints` / `debug-disassemble` 的判据是「continue 后收到
         * **≥1 次** stopped」（见两场景注释里那张判据表），**不要求多次命中**。
         * ⇒ 外层 8 次足够（第 1 次就该命中），内层只需保证「调试器连上来之前别冲过去」。
         *
         * 第一版写 200 次 × 20 万次自旋 ⇒ 实测量级是**数秒级启动延迟**
         * （200×200000×3 指令 ≈ 1.2 亿条 @108MHz），那是过度设计：
         * 它让「无调试器的板子上电后空转数秒」成为新的行为。
         * ⇒ 8 × 50000 ≈ 40 万条 ≈ 几毫秒级，既保证断点被扫到，又不改变启动手感。
         */
        for (volatile int d = 0; d < 50000; d++) { }
    }
#endif

    vTaskStartScheduler();
}
