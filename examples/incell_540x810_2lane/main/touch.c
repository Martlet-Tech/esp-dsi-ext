/*
 * touch —— 触摸子系统实现
 *
 * 协议来自原厂 STM32 驱动（AXS15260 系列），保留：
 *   - I2C 从地址 0x3B（原厂写 0x76 = 0x3B<<1）
 *   - 报点缓冲区布局：gesture + num + 每点 6 字节
 *   - 事件编码 0=DOWN 1=UP 2=CONTACT，ID 在 Y 高字节高 4 位
 *   - 无效帧判据 buf[1]>>4 != 0（原厂 touch_panel.c 同款）
 *
 * 换掉的：
 *   - 软件翻转 GPIO 的 I2C → IDF i2c_master
 *   - 中断里读整帧      → 任务里读，ISR 只给信号量
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "board_config.h"
#include "touch.h"

static const char *TAG = "TOUCH";

/* ---------------- 协议常量 ---------------- */

#define TP_MAX_POINTS     5
#define TP_ONE_POINT_LEN  6
#define TP_BUF_HEAD_LEN   2

/* 实际读多少字节。
 * 实测这颗 IC 按 1 点上报（buf[1] 低 4 位 = 1），8 字节就够。
 * 之前按 5 点读 32 字节，后 24 字节落到相邻寄存器上被当成 P2~P5，
 * 那正是"周期性重复的垃圾帧"的来源。 */
#define TP_READ_LEN       (TP_BUF_HEAD_LEN + TP_ONE_POINT_LEN)

/* 报点缓冲区是"裸读"：START + (addr|R) 直接读，不带寄存器地址。 */
#define TP_REPORT_USE_REG 0

#define TP_EV_UP          1
#define TOUCH_RELEASE_MS  150

/* ---------------- 内部状态 ---------------- */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_sem;
static TaskHandle_t            s_task;

static volatile uint16_t s_x, s_y;
static volatile bool     s_pressed;
static volatile int64_t  s_last_us;

static volatile uint32_t s_int_cnt, s_read_cnt, s_invalid_cnt, s_err_cnt;

/* ---------------- 硬件层 ---------------- */

void touch_early_pins(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOARD_TP_SCL_GPIO) |
                        (1ULL << BOARD_TP_SDA_GPIO) |
                        (1ULL << BOARD_TP_INT_GPIO),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "SCL=%d SDA=%d INT=%d -> 高阻输入",
             BOARD_TP_SCL_GPIO, BOARD_TP_SDA_GPIO, BOARD_TP_INT_GPIO);
}

static void gpio_setup(void)
{
    gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_TP_RST_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    gpio_set_level(BOARD_TP_RST_GPIO, 1);

    gpio_config_t in_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BOARD_TP_INT_GPIO,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
}

/* 复位做成公开接口，由 app_main 调用 —— 见 touch.h 的说明。
 * touch_init() 内部**不再**调它。 */
void touch_reset(void)
{
    ESP_LOGI(TAG, "TP reset (GPIO%d) H10-L20-H300", BOARD_TP_RST_GPIO);
    gpio_set_level(BOARD_TP_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TP_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

/* ---------------- 报点读取与解析 ---------------- */

static esp_err_t read_raw(uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#if TP_REPORT_USE_REG
    uint8_t zero = 0x00;
    return i2c_master_transmit_receive(s_dev, &zero, 1, buf, len, 100);
#else
    return i2c_master_receive(s_dev, buf, len, 100);
#endif
}

static inline uint16_t get_x(const uint8_t *p)  { return (uint16_t)((p[0] & 0x0F) << 8) | p[1]; }
static inline uint16_t get_y(const uint8_t *p)  { return (uint16_t)((p[2] & 0x0F) << 8) | p[3]; }
static inline uint8_t  get_ev(const uint8_t *p) { return (uint8_t)(p[0] >> 6); }

/**
 * 解析一帧。返回 true 表示是有效帧。
 *
 * 无效帧判据：buf[1] 的高 4 位非 0。
 * 合法点数是 1~5（低 4 位），高 4 位应为 0；全 FF 时高 4 位 = 0xF，一眼可辨。
 * 不做"把 15 夹成 1"之类的修正 —— 那只会把无效帧伪装成合法帧。
 */
static bool parse(const uint8_t *raw, size_t len, uint16_t *x, uint16_t *y, uint8_t *ev)
{
    if (len < TP_BUF_HEAD_LEN + TP_ONE_POINT_LEN) {
        return false;
    }
    uint8_t status = (uint8_t)(raw[1] >> 4);
    uint8_t num    = (uint8_t)(raw[1] & 0x0F);

    if (status != 0 || num == 0 || num > TP_MAX_POINTS) {
        return false;
    }
    const uint8_t *p = &raw[TP_BUF_HEAD_LEN];
    *x  = get_x(p);
    *y  = get_y(p);
    *ev = get_ev(p);
    return true;
}

/* ---------------- INT → 任务 ---------------- */

static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    s_int_cnt++;
    if (s_sem) {
        xSemaphoreGiveFromISR(s_sem, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void touch_task(void *arg)
{
    uint8_t raw[TP_READ_LEN];

    for (;;) {
        /* 100ms 超时只是为了让任务能被停掉，不是读点节拍 */
        if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        esp_err_t err = read_raw(raw, sizeof(raw));
        if (err != ESP_OK) {
            s_err_cnt++;
            if (s_err_cnt == 1 || (s_err_cnt % 50) == 0) {
                ESP_LOGE(TAG, "读报点失败 (%u 次): %s",
                         (unsigned)s_err_cnt, esp_err_to_name(err));
            }
            continue;
        }
        s_read_cnt++;

        uint16_t x, y;
        uint8_t ev;
        if (!parse(raw, sizeof(raw), &x, &y, &ev)) {
            /* 无效帧（典型是全 FF）：不动状态，靠 touch_read 的超时松手 */
            s_invalid_cnt++;
            continue;
        }

        s_x = x;
        s_y = y;
        s_pressed = (ev != TP_EV_UP);
        s_last_us = esp_timer_get_time();
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t touch_init(void)
{
    /* 复位不在这里做 —— 由 app_main 调 touch_reset()。原因见 touch.h。 */
    gpio_setup();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_TP_SDA_GPIO,
        .scl_io_num = BOARD_TP_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* 内部上拉很弱，正常板子应该有外部上拉；打开它只是让总线不至于悬空 */
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "new i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TP_I2C_ADDR,
        .scl_speed_hz = BOARD_TP_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add device");
    ESP_LOGI(TAG, "I2C 就绪: SCL=%d SDA=%d @ %dHz, addr=0x%02X",
             BOARD_TP_SCL_GPIO, BOARD_TP_SDA_GPIO, BOARD_TP_I2C_HZ, BOARD_TP_I2C_ADDR);

    /* 读一次固件版本，确认总线真的通 —— 这一行是"触摸活着"的最快证据 */
    {
        uint8_t reg = 0x0C, ver[2] = {0};
        if (i2c_master_transmit_receive(s_dev, &reg, 1, ver, 2, 100) == ESP_OK) {
            ESP_LOGW(TAG, "固件版本 = 0x%02X%02X", ver[0], ver[1]);
        } else {
            ESP_LOGE(TAG, "读版本失败 —— 总线没通，先查引脚/上拉/供电");
        }
    }

    s_sem = xSemaphoreCreateBinary();
    if (s_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(BOARD_TP_INT_GPIO, GPIO_INTR_NEGEDGE), TAG, "intr type");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_TP_INT_GPIO, int_isr, NULL), TAG, "isr add");

    if (xTaskCreate(touch_task, "touch", 4096, NULL, 6, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "INT 下降沿已挂载，读点任务启动");
    return ESP_OK;
}

bool touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    bool p = s_pressed;

    /* 超时松手：抬起后 IC 可能只发全 FF，不靠超时就会一直"按着" */
    if (p && (esp_timer_get_time() - s_last_us) > TOUCH_RELEASE_MS * 1000) {
        p = false;
        s_pressed = false;
    }

    if (x) *x = s_x;
    if (y) *y = s_y;
    if (pressed) *pressed = p;
    return p;
}

void touch_stats(touch_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->int_cnt     = s_int_cnt;
    out->read_cnt    = s_read_cnt;
    out->invalid_cnt = s_invalid_cnt;
    out->err_cnt     = s_err_cnt;
}

void touch_deinit(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_sem) {
        vSemaphoreDelete(s_sem);
        s_sem = NULL;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}
