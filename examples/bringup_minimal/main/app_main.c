/*
 * 最小点亮例程
 *
 * 目标：把一块新屏点亮，并判断"颜色对不对"。**不接 LVGL** ——
 * LVGL 会把"初始化没生效"藏起来（画面看起来有内容，但其实是错的），
 * 点亮阶段要的是最原始的证据。
 *
 * 上电后的行为：
 *   1. 初始化屏幕（电源 / 复位 / DSI / 初始化表 / video mode）
 *   2. VPG 彩条 1 秒   —— 判"链路、面板、时序"是否活
 *   3. 关背光 → 纯色轮播：红 / 绿 / 蓝 / 白 / 黑，每 2 秒一换
 *                        —— 判颜色。串口同步打印"这一帧应该是什么"
 *
 * 判读（红的时候看屏幕）：
 *   红     → 全对
 *   蓝     → R/B 通道顺序反了（检查 framebuffer 是不是按 B,G,R 写的）
 *   青     → 整屏反相（每个通道取反）
 *   粉/暗  → 位打包或位宽问题，先查 porch 和像素格式
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "screen.h"

static const char *TAG = "APP";

typedef struct {
    uint8_t r, g, b;
    const char *name;
} color_t;

static const color_t k_colors[] = {
    { 0xFF, 0x00, 0x00, "RED"   },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
    { 0xFF, 0xFF, 0xFF, "WHITE" },
    { 0x00, 0x00, 0x00, "BLACK" },
};
#define COLOR_CNT (sizeof(k_colors) / sizeof(k_colors[0]))

#define STEP_MS  2000

void app_main(void)
{
    ESP_LOGW(TAG, "==== bringup_minimal ====");

    esp_err_t err = screen_init();
    if (err != ESP_OK) {
        /* 不 ESP_ERROR_CHECK：初始化失败时停在这里，日志已经说明了哪一步，
         * 重启只会把现场冲掉。 */
        ESP_LOGE(TAG, "screen_init 失败: %s —— 停在这里，请看上面最后一条日志",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "screen_init OK: %ux%u", screen_width(), screen_height());

    /* ---- VPG 彩条：判链路/面板/时序 ---- */
    /* ★ 注意边界：VPG 也要过 host 的打包器，porch 错时它一样错。
     *   所以"VPG 正常"只能说明几何和出图正常，不能证明颜色对。 */
    if (screen_vpg(1) == ESP_OK) {
        ESP_LOGW(TAG, "VPG 垂直彩条 1 秒（判几何/是否出图，不判颜色）");
        vTaskDelay(pdMS_TO_TICKS(1000));
        screen_vpg(0);   /* 必须关掉，否则后面的 framebuffer 内容不上屏 */
    } else {
        ESP_LOGW(TAG, "VPG 不可用，跳过");
    }

    screen_backlight(true);

    /* ---- 纯色轮播：判颜色 ---- */
    for (int i = 0;; i = (i + 1) % COLOR_CNT) {
        const color_t *c = &k_colors[i];
        err = screen_fill(c->r, c->g, c->b);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "%s  (R=%02X G=%02X B=%02X) —— 屏幕应显示 %s",
                     c->name, c->r, c->g, c->b, c->name);
        } else {
            ESP_LOGE(TAG, "screen_fill 失败: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}
