/*
 * screen —— 显示子系统
 *
 * 对外只暴露"我有一块屏，能初始化、能开关背光、能拿到句柄和尺寸"，
 * **不 include lvgl.h**，也不 include 任何 hal/寄存器头。
 * 上层（app_main / lv_port）只通过这里和屏幕打交道。
 *
 * 依赖方向：
 *     app_main ──> screen ──> dsi_ext / esp_lcd
 *              ──> touch
 *              ──> lv_port ──> screen_panel()  (只拿句柄)
 * 禁止反向依赖：screen 不认识 LVGL，也不认识 touch。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

/**
 * @brief 上电（幂等）：LCD 电源 → MIPI 模拟开关 → DPHY LDO → 背光关
 *
 * 单独暴露出来是因为有时需要在"上电"和"复位"之间插板级操作 ——
 * 比如把触摸的 SCL/SDA/INT 先配成高阻。screen_init() 内部会自动调用它，
 * 所以不需要时可以不显式调。
 */
esp_err_t screen_power_on(void);

/**
 * @brief LCD 复位（H → L → H），由 app_main 调用
 *
 * **为什么单独暴露，而不放在 screen_init() 里：**
 *
 *   - 复位时机和顺序是板级/IC 级的事。incell TDDI 要求 TP 复位在 LCD 复位
 *     **之后**，有些 IC 还要求在两者之间插延时或别的动作。写在 app_main 里
 *     一眼能看见顺序；藏进模块内部，读代码的人得翻两个文件才知道。
 *   - 调试时经常需要单独重放某一路复位（例如只重放 TP 复位看 INT 恢复没有）。
 *   - 重建链路时（screen_destroy → screen_init）需要调用方决定要不要补一次复位。
 *
 * screen_init() **不会**调用本函数。上电流程由 app_main 显式编排：
 *
 *     screen_power_on();
 *     touch_early_pins();
 *     screen_reset();       // LCD 复位
 *     touch_reset();        // TP 复位（必须在 LCD 之后）
 *     screen_init();        // DSI + 初始化表 + video mode
 *     touch_init();         // I2C + INT + 读点任务
 *
 * 如果某块屏的上电时序不同，改 app_main 的调用顺序即可，不用动模块。
 */
void screen_reset(void);

/**
 * @brief 初始化 + 启动出图（**不含复位**）
 *
 * 顺序（每一步失败都会返回错误，不会静默继续）：
 *   1. LCD 电源（若配置了引脚）      ← 若已 screen_power_on() 则跳过
 *   2. MIPI 模拟开关（若配置了）
 *   3. DPHY LDO
 *   4. 背光关（避免在没出图时闪白）
 *   5. DSI 总线
 *   6. 命令通道：ACK 关、写命令走 LP
 *   7. DPI 面板对象（此时还没出图）
 *   8. 发初始化表
 *   9. esp_lcd_panel_init() → video mode 启动
 *  10. 分配 framebuffer（PSRAM）
 *
 * 返回后屏幕已经在扫描，但背光还关着 —— 调 screen_backlight(true) 点亮。
 * 这样"出图"和"亮灯"分开，出图失败时屏幕不会先闪一片白。
 *
 * 可重复调用（先 screen_destroy() 再调）。为什么需要：DSI 读一旦卡死，
 * host 内部状态软件清不掉，只能整体掉电重建。
 */
esp_err_t screen_init(void);

/** @brief 释放显示资源（面板 → 链路复位 → 总线） */
void screen_destroy(void);

/** @brief 背光开关 */
esp_err_t screen_backlight(bool on);

/** @brief 把整屏填成一种颜色（r/g/b 各 8bit），用来自检 */
esp_err_t screen_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 让 DSI host 直接输出内置测试图案（VPG）
 *
 * 图案由 host 内部生成，不经过 framebuffer，也不经过 bridge 的 DPI 输入通路。
 * 用它可以把"链路/面板/时序对不对"和"我们送的数据对不对"分开。
 *
 * ★ 边界：VPG **也能反映 porch 错误**（它要过 host 的打包器）。所以
 *   "VPG 正常"只能说明几何和出图正常，不能说明颜色正确。
 *   判颜色要用 screen_fill() 出纯色。
 *
 * @param pattern 0=关，1=垂直彩条，2=水平彩条，3=误码率图案
 */
esp_err_t screen_vpg(int pattern);

/** @brief 面板句柄，交给 lv_port 用 */
esp_lcd_panel_handle_t screen_panel(void);

uint16_t screen_width(void);
uint16_t screen_height(void);
