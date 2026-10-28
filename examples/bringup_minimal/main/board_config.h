/*
 * 板级配置 —— 引脚、电源、背光的**唯一**一处。
 *
 * 换板子只改这个文件。屏幕本身的参数（分辨率/porch/lane）在 panel_timing.h，
 * 初始化命令在 panel_init.c，三者互不重叠。
 *
 * 引脚写 -1 表示"这块板没有这根线"，代码会跳过，不会误操作别的 GPIO。
 */
#pragma once

/* ---------------- LCD ---------------- */

/** LCD 电源使能脚（高电平开）。没有独立电源控制就写 -1。 */
#define BOARD_LCD_POWER_GPIO     (-1)

/** LCD 复位的 GPIO，写 -1 表示复位由硬件 RC 完成。 */
#define BOARD_LCD_RST_GPIO       (-1)

/** 背光使能脚。写 -1 表示这块板背光常亮 / 由别的芯片控制。 */
#define BOARD_BK_LIGHT_GPIO      (-1)

/** 背光有效电平：1 = 高电平点亮 */
#define BOARD_BK_LIGHT_ACTIVE    1

/* ---------------- MIPI 模拟开关（可选） ---------------- */
/* 有些板在 DSI 线上串了模拟开关，需要在出图前切到 MIPI 通路。
 * 没有就都写 -1。 */
#define BOARD_MIPI_SW_EN_GPIO    (-1)
#define BOARD_MIPI_SW_SEL_GPIO   (-1)
#define BOARD_MIPI_SW_EN_LEVEL   0
#define BOARD_MIPI_SW_SEL_LEVEL  1

/* ---------------- MIPI DPHY 供电 ---------------- */
/* P4 的 VDD_MIPI_DPHY 由片内 LDO 提供，用 esp_ldo_acquire_channel() 打开。
 * chan_id 一般是 3；电压按模组规格，常见 1800 / 2500 mV。
 * 写 BOARD_PHY_LDO_CHAN 为 -1 表示不用片内 LDO（外部已供电）。 */
#define BOARD_PHY_LDO_CHAN       3
#define BOARD_PHY_LDO_MV         2500

/* ---------------- 时序余量 ---------------- */
/* 上电到能发命令的等待，以及复位后的等待。不同面板差异很大，
 * 起不来时先把这里放大一倍再试。 */
#define BOARD_POWER_SETTLE_MS    50
#define BOARD_RESET_LOW_MS       20
#define BOARD_RESET_AFTER_MS     120
