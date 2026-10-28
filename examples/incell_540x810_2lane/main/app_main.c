/*
 * incell 540x810 · 2 lane · RGB888 —— 完整参考例程
 *
 * 这块屏是 incell TDDI：触摸和显示在同一颗 die 上，所以两者的复位
 * 是同一件事的一部分，顺序不能乱。
 *
 * app_main 只做组装，不碰任何 hal / 寄存器头：
 *
 *   screen_power_on()   上电（电源、MIPI 开关、DPHY LDO、背光关）
 *   touch_early_pins()  触摸三脚进高阻，让模组上拉决定总线电平
 *   screen_init()       复位 + DSI + 初始化表 + video mode
 *   touch_init()        触摸复位 + I2C + INT 任务（必须在显示初始化之后）
 *   lv_port_init()      LVGL：显示 + 输入 + 节拍
 *   lv_demo_music()     LVGL 自带 demo
 *   lv_port_start()     起 LVGL 任务
 *   screen_backlight()  最后点灯
 *
 * 每一步的顺序都有理由，改之前先看对应模块的注释。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "screen.h"
#include "touch.h"
#include "lv_port.h"

#include "lvgl.h"
/* LVGL 组件只把 <lvgl>/demos 加进 include 路径，头文件在 demos/music/ 下，
 * 所以要带 music/ 前缀 */
#include "music/lv_demo_music.h"

static const char *TAG = "APP";

/* 开机先出 1 秒 VPG 彩条再进 LVGL。判"链路/面板/时序是否正常"用。
 * 不想看就置 0。 */
#define VPG_BOOT_TEST   1

/* 显示初始化完成到触摸初始化之间必须留的空隙。
 * 原厂资料：TP 寄存器读写要在显示初始化之后，且与 11h/29h 间隔 >100ms。 */
#define TP_INIT_DELAY_MS  100

void app_main(void)
{
    ESP_LOGW(TAG, "==== incell 540x810 / 2 lane / RGB888 ====");

    /* ---- 1) 上电 ---- */
    ESP_ERROR_CHECK(screen_power_on());

    /* ---- 2) 触摸三脚先进高阻 ----
     * 从给电到显示初始化完成这段（含两次复位）里，P4 侧对触摸总线必须透明。 */
    touch_early_pins();

    /* ---- 3) 屏幕 ---- */
    esp_err_t err = screen_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen_init 失败: %s —— 停在这里，看上面最后一条日志",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "screen OK: %ux%u", screen_width(), screen_height());

    /* ---- 4) VPG 自检（可选） ---- */
#if VPG_BOOT_TEST
    /* 图案由 host 内部生成，不经 framebuffer —— 能快速判断"面板/时序是否活"。
     * 注意它也要过 host 的打包器，所以"彩条正常"不代表颜色一定对。 */
    if (screen_vpg(1) == ESP_OK) {
        ESP_LOGW(TAG, "VPG 垂直彩条 1 秒");
        vTaskDelay(pdMS_TO_TICKS(1000));
        screen_vpg(0);   /* 必须关，否则 draw_bitmap 不上屏 */
    }
#endif

    /* ---- 5) 触摸（必须在显示初始化之后） ---- */
    vTaskDelay(pdMS_TO_TICKS(TP_INIT_DELAY_MS));
    err = touch_init();
    if (err != ESP_OK) {
        /* 触摸失败不影响显示，继续往下走 */
        ESP_LOGE(TAG, "touch_init 失败: %s（显示继续）", esp_err_to_name(err));
    }

    /* ---- 6) LVGL ---- */
    if (lv_port_init(screen_panel(), screen_width(), screen_height()) == ESP_OK) {
        if (lv_port_lock(1000)) {
            lv_demo_music();
            lv_port_unlock();
            ESP_LOGW(TAG, "lv_demo_music 已创建");
        }
        lv_port_start();
    } else {
        ESP_LOGE(TAG, "LVGL 初始化失败");
    }

    /* ---- 7) 点灯 ---- */
    screen_backlight(true);

    /* ---- 8) 心跳 ---- */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        touch_stats_t st;
        touch_stats(&st);
        ESP_LOGI(TAG, "alive | INT=%u 读取=%u 无效=%u 出错=%u | heap=%uKB psram=%uKB",
                 (unsigned)st.int_cnt, (unsigned)st.read_cnt,
                 (unsigned)st.invalid_cnt, (unsigned)st.err_cnt,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
