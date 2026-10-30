/*
 * screen —— 显示子系统的全部实现
 *
 * 分层：本文件之后是 dsi_ext / esp_lcd；本文件之前只认 screen.h 的五个函数。
 * 所有寄存器级的脏活（任意 DT 发包、读回、VPG）都交给 dsi_ext 组件，
 * 这里不直接 include 任何 hal 头 —— 换 IDF 版本时只需要复查组件。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#include "dsi_ext.h"
#include "board_config.h"
#include "panel_timing.h"
#include "panel_init.h"
#include "screen.h"

static const char *TAG = "SCR";

/* 每像素字节数。RGB666 在内存里也占 3 字节（只用高 6 位），所以按 3 算。 */
#if PANEL_BITS_PER_PIXEL == 16
#define SCR_BYTES_PER_PIXEL  2
#define SCR_COLOR_FMT        LCD_COLOR_FMT_RGB565
#elif PANEL_BITS_PER_PIXEL == 18
#define SCR_BYTES_PER_PIXEL  3
#define SCR_COLOR_FMT        LCD_COLOR_FMT_RGB666
#elif PANEL_BITS_PER_PIXEL == 24
#define SCR_BYTES_PER_PIXEL  3
#define SCR_COLOR_FMT        LCD_COLOR_FMT_RGB888
#else
#error "PANEL_BITS_PER_PIXEL 只能是 16 / 18 / 24"
#endif

/* 发完初始化表后读一次 0x0A (RDDPM) 验证。置 0 可跳过。
 * 这是"初始化到底生效没有"最硬的证据，建议保留。 */
#define SCR_READ_RDDPM   1

static esp_lcd_dsi_bus_handle_t s_bus;
static esp_lcd_panel_handle_t   s_panel;
static uint8_t                 *s_fb;
static esp_ldo_channel_handle_t s_ldo;
static bool                     s_powered;

/* ------------------------------ 板级上电 ------------------------------ */

static void gpio_out(int pin, int level)
{
    if (pin < 0) {
        return;
    }
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << pin,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(pin, level);
}

esp_err_t screen_power_on(void)
{
    if (s_powered) {
        return ESP_OK;
    }

    gpio_out(BOARD_LCD_POWER_GPIO, 1);
    if (BOARD_LCD_POWER_GPIO >= 0) {
        ESP_LOGI(TAG, "[1] LCD power on (GPIO%d)", BOARD_LCD_POWER_GPIO);
        vTaskDelay(pdMS_TO_TICKS(BOARD_POWER_SETTLE_MS));
    }

    if (BOARD_MIPI_SW_EN_GPIO >= 0 || BOARD_MIPI_SW_SEL_GPIO >= 0) {
        gpio_out(BOARD_MIPI_SW_EN_GPIO, BOARD_MIPI_SW_EN_LEVEL);
        gpio_out(BOARD_MIPI_SW_SEL_GPIO, BOARD_MIPI_SW_SEL_LEVEL);
        ESP_LOGI(TAG, "[1] MIPI switch: EN=%d SEL=%d",
                 BOARD_MIPI_SW_EN_LEVEL, BOARD_MIPI_SW_SEL_LEVEL);
    }

#if BOARD_PHY_LDO_CHAN >= 0
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_PHY_LDO_CHAN,
        .voltage_mv = BOARD_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo), TAG, "ldo acquire");
    ESP_LOGI(TAG, "[1] VDD_MIPI_DPHY = %d mV", BOARD_PHY_LDO_MV);
#endif

    /* 背光先关：没出图就点亮会先闪一屏白 */
    gpio_out(BOARD_BK_LIGHT_GPIO, !BOARD_BK_LIGHT_ACTIVE);

    s_powered = true;
    return ESP_OK;
}

/* 复位做成公开接口，由 app_main 调用 —— 见 screen.h 的说明。
 * screen_init() 内部**不再**调它，保证调用点唯一、可见。 */
void screen_reset(void)
{
    if (BOARD_LCD_RST_GPIO < 0) {
        ESP_LOGW(TAG, "未配置复位脚 (BOARD_LCD_RST_GPIO=-1)，跳过");
        return;
    }
    ESP_LOGI(TAG, "LCD reset (GPIO%d) H/L%dms/H%ddms",
             BOARD_LCD_RST_GPIO, BOARD_RESET_LOW_MS, BOARD_RESET_AFTER_MS);

    gpio_out(BOARD_LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOARD_RESET_LOW_MS));
    gpio_set_level(BOARD_LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOARD_RESET_AFTER_MS));
}

/* ------------------------------ 初始化表 ------------------------------ */

static esp_err_t send_panel_init(void)
{
    ESP_LOGI(TAG, "[9] 初始化表 %d 条", g_panel_init_count);
    int failed = 0;

    for (int i = 0; i < g_panel_init_count; i++) {
        const panel_cmd_t *c = &g_panel_init[i];
        esp_err_t err = dsi_ext_tx(0, c->dt, c->data, c->len);
        if (err != ESP_OK) {
            /* 打出来而不是 ESP_ERROR_CHECK：一条失败不代表整屏废，
             * 而且停下能看到后面还有多少条没发。 */
            ESP_LOGE(TAG, "  #%02d DT=%02X cmd=%02Xh len=%2d FAIL %s",
                     i, c->dt, c->data[0], c->len, esp_err_to_name(err));
            failed++;
        } else {
            ESP_LOGI(TAG, "  #%02d DT=%02X cmd=%02Xh len=%2d ok",
                     i, c->dt, c->data[0], c->len);
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    if (failed) {
        ESP_LOGE(TAG, "  %d/%d 条失败 —— 命令通道有问题, 先别怀疑面板",
                 failed, g_panel_init_count);
        return ESP_FAIL;
    }
    return ESP_OK;
}

#if SCR_READ_RDDPM
/* 读 0x0A RDDPM。11h + 13h + 29h 之后应当是 0x9C：
 *   bit7 booster | bit4 sleep_out | bit3 normal | bit2 display_on
 * 读到 0x00 / 0xFF / 随机值说明读通路没通。 */
static void read_rddpm(void)
{
    uint8_t v = 0;
    uint16_t n = 0;
    esp_err_t err = dsi_ext_rx_dcs(0x0A, &v, 1, 100, &n);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[10] 读 0x0A 失败: %s (面板不支持读就忽略这条)",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "[10] 读 0x0A = 0x%02X (%u 字节) | booster=%d sleep_out=%d "
             "normal=%d display_on=%d",
             v, n, (v >> 7) & 1, (v >> 4) & 1, (v >> 3) & 1, (v >> 2) & 1);
}
#endif

/* ------------------------------ 对外接口 ------------------------------ */

esp_err_t screen_init(void)
{
    /* 复位不在这里做 —— 由 app_main 调 screen_reset()。
     * 原因见 screen.h：复位时机和顺序是板级/IC 级的事，藏在模块内部就不可见了。 */
    ESP_RETURN_ON_ERROR(screen_power_on(), TAG, "power on");

    /* ---- DSI 总线 ---- */
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = PANEL_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = PANEL_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus), TAG, "new_dsi_bus");
    ESP_LOGI(TAG, "[3] DSI bus: %d lane @ %d Mbps", PANEL_LANE_NUM, PANEL_LANE_MBPS);

    /* ---- 命令通道 ----
     * 不用 esp_lcd_panel_io 的发命令接口（它把 DT 写死成 DCS），
     * 所以它本该顺手做的事要自己设：
     *   cmd_ack = off —— IDF 默认打开，而 bus 层把超时全设 0，
     *                    面板不回 ACK 就永久等总线转向，task_wdt。
     *   写命令走 LP   —— 便宜的 TDDI 通常只实现 LP 命令入口。 */
    ESP_RETURN_ON_ERROR(dsi_ext_init(0), TAG, "dsi_ext_init");
    ESP_RETURN_ON_ERROR(dsi_ext_cmd_channel_config(false, false,
                                                   DSI_EXT_WR_SPEED_LP),
                        TAG, "cmd channel");
    ESP_LOGI(TAG, "[4] cmd_ack=OFF, 写命令走 LP");

    /* 读请求的速度和写是**分开的两位**，IDF 从不设，复位值是 HS。
     * 面板只收 LP 时读不通 —— 而且后果是命令 FIFO 死锁、黑屏、不可恢复。
     * 一行设好，代价为零。 */
    ESP_RETURN_ON_ERROR(dsi_ext_set_read_speed(true), TAG, "read speed LP");

    /* ---- DPI 面板对象（此时还没开始出图）---- */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_DPI_CLK_MHZ,
        .in_color_format = SCR_COLOR_FMT,
        .num_fbs = 1,
        .video_timing = {
            .h_size = PANEL_H_SIZE,
            .v_size = PANEL_V_SIZE,
            .hsync_pulse_width = PANEL_HSW,
            .hsync_back_porch = PANEL_HBP,
            .hsync_front_porch = PANEL_HFP,
            .vsync_pulse_width = PANEL_VSW,
            .vsync_back_porch = PANEL_VBP,
            .vsync_front_porch = PANEL_VFP,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_bus, &dpi_cfg, &s_panel), TAG, "new_panel_dpi");

    {
        double fps = (double)PANEL_DPI_CLK_MHZ * 1e6 /
                     ((double)PANEL_HTOTAL * (double)PANEL_VTOTAL);
        ESP_LOGI(TAG, "[5] %ux%u  HTOTAL=%d VTOTAL=%d  DPI=%dMHz  ->  %.1f Hz  "
                 "行周期 %.2f us",
                 PANEL_H_SIZE, PANEL_V_SIZE, PANEL_HTOTAL, PANEL_VTOTAL,
                 PANEL_DPI_CLK_MHZ, fps, (double)PANEL_HTOTAL / PANEL_DPI_CLK_MHZ);
    }

    /* 极性：IDF 默认全 active-high，这里按 panel_timing.h 覆盖。
     * 注意要在 new_panel_dpi 之后、panel_init 之前设，panel_init 内部会重设。 */
    ESP_RETURN_ON_ERROR(dsi_ext_set_timing_polarity(PANEL_HSYNC_LOW, PANEL_VSYNC_LOW,
                                                    PANEL_DE_LOW, false, false),
                        TAG, "polarity");

    /* ---- 初始化表：必须在视频模式启动之前发 ---- */
    if (send_panel_init() != ESP_OK) {
        return ESP_FAIL;
    }

#if SCR_READ_RDDPM
    read_rddpm();
#endif

    /* ---- 启动 video mode（对应 SSD2828 的 MIPI_VIDEO()）---- */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init(video mode)");
    ESP_LOGW(TAG, "[11] video mode 已启动");

    /* ---- framebuffer（自检用，PSRAM）---- */
    size_t fb_size = (size_t)PANEL_H_SIZE * PANEL_V_SIZE * SCR_BYTES_PER_PIXEL;
    s_fb = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer 分配失败 (%u 字节)", (unsigned)fb_size);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0x00, fb_size);
    ESP_LOGI(TAG, "[12] framebuffer %u 字节 @ %p", (unsigned)fb_size, s_fb);

    return ESP_OK;
}

void screen_destroy(void)
{
    if (s_fb) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    /* 关键顺序：先掉电复位数字逻辑，再释放总线。
     * 只在"读卡死"这类需要重建的场景用得上（TRM 43.4.3.1.2）。 */
    dsi_ext_link_reset();
    if (s_bus) {
        esp_lcd_del_dsi_bus(s_bus);
        s_bus = NULL;
    }
}

esp_err_t screen_backlight(bool on)
{
    if (BOARD_BK_LIGHT_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* gpio_out 会重新 gpio_config，这里已经配过，直接写电平即可 */
    gpio_set_level(BOARD_BK_LIGHT_GPIO, on ? BOARD_BK_LIGHT_ACTIVE : !BOARD_BK_LIGHT_ACTIVE);
    ESP_LOGW(TAG, "backlight %s", on ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t screen_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_panel == NULL || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int n = PANEL_H_SIZE * PANEL_V_SIZE;
#if SCR_BYTES_PER_PIXEL == 2
    /* RGB565 */
    uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    uint16_t *p = (uint16_t *)s_fb;
    for (int i = 0; i < n; i++) {
        p[i] = px;
    }
#else
    /* RGB888 / RGB666：内存排列是 B,G,R（TRM 43.4.1），不是 R,G,B。
     * 这一点非常容易搞错 —— 按 R,G,B 写会得到红蓝互换的画面。 */
    for (int i = 0; i < n; i++) {
        uint8_t *p = s_fb + i * 3;
        p[0] = b;
        p[1] = g;
        p[2] = r;
    }
#endif

    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_H_SIZE, PANEL_V_SIZE, s_fb);
}

esp_err_t screen_vpg(int pattern)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return dsi_ext_vpg(s_panel, (dsi_ext_pattern_t)pattern);
}

esp_lcd_panel_handle_t screen_panel(void)
{
    return s_panel;
}

uint16_t screen_width(void)
{
    return PANEL_H_SIZE;
}

uint16_t screen_height(void)
{
    return PANEL_V_SIZE;
}
