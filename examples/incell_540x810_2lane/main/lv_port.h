/*
 * lv_port —— LVGL 9 移植层（显示 + 输入 + 节拍 + 任务）
 *
 * 这是整个工程里唯一"两边都认识"的模块：它知道 LVGL 的 API，
 * 也知道 screen 的句柄和 touch 的读数接口。screen 和 touch 本身
 * 完全不知道 LVGL 存在。
 *
 * 显示：PARTIAL 模式，两块局部缓冲放 PSRAM，flush 直接 draw_bitmap。
 * 输入：indev 回调只调 touch_read()，不碰 I2C、不阻塞。
 * 节拍：lv_tick_set_cb 用 esp_timer_get_time()/1000，不需要额外定时器。
 * 线程：LVGL 不是线程安全的。只有 lvgl_task 在持锁时跑 lv_timer_handler，
 *       其它任务要动 UI 必须先 lv_port_lock()。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

/**
 * @brief 初始化 LVGL，创建 display 和 indev
 * @param panel  DPI 面板句柄（来自 screen_panel()）
 * @param w,h    面板分辨率（来自 screen_width()/screen_height()）
 */
esp_err_t lv_port_init(esp_lcd_panel_handle_t panel, int w, int h);

/** @brief 启动 LVGL 任务（每 10ms 一次 lv_timer_handler） */
void lv_port_start(void);

/** @brief 持锁访问 LVGL。创建界面时必须包起来。 */
bool lv_port_lock(uint32_t timeout_ms);
void lv_port_unlock(void);
