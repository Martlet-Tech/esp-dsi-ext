/*
 * touch —— 触摸子系统
 *
 * 对外只有三件事：初始化、取当前触点、释放。
 * **不 include lvgl.h，也不 include screen.h** —— 触摸不知道自己接在哪块屏上。
 *
 * 内部结构（INT 脉冲驱动，不轮询）：
 *
 *   TP INT 下降沿 ──ISR(只给信号量, 不碰 I2C)──> touch_task
 *                                                  └─ 读 8 字节 → 解析 → 写共享状态
 *   touch_read() ── 只读共享状态，不阻塞、不碰总线
 *
 * 为什么不在 ISR 里读：原厂 STM32 例程就是在 EXTI 回调里读完整帧再刷屏，
 * 仓库自己的注释写着 "process time is too long, leading to lost interrupt
 * process"。P4 上 1MHz I2C 读 8 字节约 200us，放任务里更稳。
 *
 * 为什么不轮询：无触摸时完全不碰总线。之前 20ms 轮询会在 IC 内部指针错位时
 * 读到一堆垃圾帧（num=5、全 FF），过滤掉也很难看。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief 上电后立刻调用：把 SCL/SDA/INT 配成高阻输入（不开上下拉）
 *
 * 从给电到显示初始化完成这段时间里，P4 侧对触摸总线必须"透明"，
 * 总线电平由模组侧上拉决定。芯片复位默认值本来就是这样，但那是隐含假设；
 * 这里把它变成显式约定。
 *
 * 调用位置：屏幕上电之后、LCD 复位之前。
 */
void touch_early_pins(void);

/**
 * @brief 初始化触摸：复位 + I2C + 挂 INT + 起读取任务
 *
 * ★ 必须在显示初始化**完成之后**调用，并且与 LCD 发 11h/29h 间隔 >100ms。
 *   原厂资料明确要求：incell 屏的触摸与显示同一颗 die，显示没初始化完就
 *   读写触摸寄存器，行为不确定。
 */
esp_err_t touch_init(void);

/**
 * @brief 取当前触点（非阻塞，只读共享状态）
 *
 * @param x,y       输出坐标，可为 NULL
 * @param pressed   输出按下状态，可为 NULL
 * @return true = 有有效触点（pressed 可能为 false，表示刚抬起）
 *
 * 超过 TOUCH_RELEASE_MS 没有新报点会自动按"已抬起"处理 —— IC 抬起后
 * 可能不再上报有效帧，只发全 FF，不靠超时就会一直按着。
 */
bool touch_read(uint16_t *x, uint16_t *y, bool *pressed);

/** @brief 停止任务并释放 I2C（为可重复 bringup 预留） */
void touch_deinit(void);

/** @brief 读点统计，用来判断"INT 在跳但读不出来"这类问题 */
typedef struct {
    uint32_t int_cnt;      /* INT 下降沿次数 */
    uint32_t read_cnt;     /* 成功读回的次数 */
    uint32_t invalid_cnt;  /* 读回但被判无效（全 FF 类）的次数 */
    uint32_t err_cnt;      /* I2C 出错次数 */
} touch_stats_t;

void touch_stats(touch_stats_t *out);
