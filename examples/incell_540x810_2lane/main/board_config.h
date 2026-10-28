/*
 * 板级配置 —— 引脚、电源、背光的**唯一**一处。
 *
 * 本文件对应实际硬件（540x810 incell TDDI 模组 + 一颗 P4 转接板）。
 * 换板子只改这里。屏幕时序在 panel_timing.h，初始化命令在 panel_init.c。
 */
#pragma once

/* ---------------- LCD ---------------- */

#define BOARD_LCD_POWER_GPIO     52     /* 高电平给屏幕上电 */
#define BOARD_LCD_RST_GPIO       23
#define BOARD_BK_LIGHT_GPIO      18     /* 高电平点亮 */
#define BOARD_BK_LIGHT_ACTIVE    1

/* ---------------- MIPI 模拟开关 ---------------- */
/* 这块板上 DSI 线串了一个模拟开关，EN=0 / SEL=1 才走 MIPI 通路。 */
#define BOARD_MIPI_SW_EN_GPIO    30
#define BOARD_MIPI_SW_SEL_GPIO   31
#define BOARD_MIPI_SW_EN_LEVEL   0
#define BOARD_MIPI_SW_SEL_LEVEL  1

/* ---------------- MIPI DPHY 供电 ---------------- */
/* 片内 LDO3 给 VDD_MIPI_DPHY。这块屏在 1800mV 下稳定；
 * 若出现偶发花屏/整帧噪点，先回 2500mV（IDF 官方例程的值）对照。 */
#define BOARD_PHY_LDO_CHAN       3
#define BOARD_PHY_LDO_MV         1800

/* ---------------- 时序余量 ---------------- */

#define BOARD_POWER_SETTLE_MS    50
#define BOARD_RESET_LOW_MS       20
#define BOARD_RESET_AFTER_MS     120

/* ---------------- 触摸 ---------------- */
/* incell 屏：触摸与显示同一颗 die，所以 TP 复位和 LCD 复位是同一件事的一部分。
 * 顺序必须是：给电 → 引脚高阻 → LCD 复位 → TP 复位 → 发初始化表。 */
#define BOARD_TP_SCL_GPIO        22
#define BOARD_TP_SDA_GPIO        21
#define BOARD_TP_INT_GPIO        20
#define BOARD_TP_RST_GPIO        19

/* I2C 从地址（7bit）。原厂资料写 0x76 是含读写位的 8bit 形式，右移一位 = 0x3B。 */
#define BOARD_TP_I2C_ADDR        0x3B

/* 1MHz。这块板实测可用；若换成上拉更弱的板子，先降到 400kHz。 */
#define BOARD_TP_I2C_HZ          1000000
