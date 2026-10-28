/*
 * lv_port —— LVGL 9 移植层实现。设计说明见 lv_port.h。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"
#include "touch.h"
#include "lv_port.h"

static const char *TAG = "LV";

/* 局部缓冲行数：540 x 100 x 3 = 162KB/块，两块 324KB，放 PSRAM。
 * 行数越多 flush 次数越少；100 行对 540x810 是 9 次全屏 flush。 */
#define LV_BUF_LINES        100
#define LV_TASK_PERIOD_MS   10

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_indev_t   *s_indev;
static SemaphoreHandle_t s_mutex;

static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 关于 R/B 交换：**不要做**。
 *
 * P4 TRM 43.4.1：RGB888 时 framebuffer 的内存排列是 B0 G0 R0（最低字节是蓝）。
 * 而 LVGL 的 lv_color_t 定义就是 {blue, green, red}，天然与硬件一致。
 * 在 flush 里"贴心"地交换 R/B 反而会让整屏反色 —— 这是踩过的坑。
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* PARTIAL 模式下 px_map 就是这块区域的连续像素，直接交给面板。
     * esp_lcd_panel_draw_bitmap 的 end 是排他的，所以 +1。 */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    bool pressed = false;

    /* touch_read 只读共享状态，不阻塞、不碰总线。
     * 它内部已经处理了"超过 150ms 没有新报点就当抬起"。 */
    touch_read(&x, &y, &pressed);

    data->point.x = (int32_t)x;
    data->point.y = (int32_t)y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t lv_port_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    s_panel = panel;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "mutex 创建失败");
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    size_t buf_bytes = (size_t)w * LV_BUF_LINES * 3;
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "绘制缓冲分配失败 (%u B x2)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_disp = lv_display_create(w, h);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "display 创建失败");
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);

    ESP_LOGW(TAG, "LVGL %d.%d.%d 就绪: %dx%d RGB888, 局部缓冲 %d 行 x2 (%u KB)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             w, h, LV_BUF_LINES, (unsigned)(buf_bytes * 2 / 1024));
    return ESP_OK;
}

bool lv_port_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lv_port_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void lvgl_task(void *arg)
{
    ESP_LOGW(TAG, "LVGL 任务启动: 每 %d ms 一次 lv_timer_handler", LV_TASK_PERIOD_MS);

    for (;;) {
        if (lv_port_lock(50)) {
            lv_timer_handler();
            lv_port_unlock();
        } else {
            ESP_LOGW(TAG, "取锁超时，跳过一轮");
        }
        vTaskDelay(pdMS_TO_TICKS(LV_TASK_PERIOD_MS));
    }
}

void lv_port_start(void)
{
    /* 优先级低于 touch 的读点任务，高于 idle；栈给大一些，
     * music demo 的布局递归比较深。 */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);
}
