# esp-dsi-ext

**不用改 ESP-IDF，就能用上 `esp_lcd` 没有暴露出来的 MIPI DSI 能力。**

ESP-IDF 的 `esp_lcd` 只覆盖"正常路径"：DPI 面板 + DCS 命令 + 固定极性 + 不能读。
但真实面板经常需要更多。这些能力其实都在 `hal` 层躺着（全是 `static inline` 的寄存器
封装），只是没有对上层暴露。

本组件在自己的编译单元里包含那些私有头文件，直接调用它们 —— **一行 IDF 源码都不用改**，
兄弟工程可以共用同一份干净的 IDF，也不用维护多份 patch。

---

## 它解决什么问题

| 能力 | ESP-IDF 的现状 | esp-dsi-ext |
|---|---|---|
| 任意 DT 的命令发送 | `esp_lcd_panel_io_tx_param()` 把 DT 写死成 DCS | `dsi_ext_tx()` 支持 `0x03/0x05/0x13/0x15/0x29/0x39` |
| 从面板读回数据 | 只发 DCS Read(`0x06`)，三处裸 `while` 无超时，恒返回 `ESP_OK`，读完不恢复 video mode | `dsi_ext_rx()` 任意 DT + 超时 + 返回实际长度 + 恢复现场 |
| 读请求的传输速度 | 复位值 HS，IDF 从不设置 | `dsi_ext_set_read_speed()` ← **很多屏靠这一位才能读通** |
| 命令 FIFO 等待 | 裸 `while`，面板不回应就 task_wdt | 带次数上限，超时返回 `ESP_ERR_TIMEOUT` |
| 命令 ACK / 传输速度 | `io_dbi.c` 打开 cmd_ack，速度写死 | `dsi_ext_cmd_channel_config()` |
| 同步极性 | `dpi_panel_init()` 全写 active-high | `dsi_ext_set_timing_polarity()` |
| clock lane 状态 | 固定 AUTO（消隐期回 LP） | `dsi_ext_set_clock_lane()` |
| 像素排布 | `pixel_type.dpi_config` 永远写 0 | `dsi_ext_set_pixel_type()` |
| 内置测试图案 | `esp_lcd_dpi_panel_set_pattern()` 有，但容易忘关 | `dsi_ext_vpg()`，自带 OFF 语义 |
| 直写面板内部 framebuffer | 只能走 `draw_bitmap` 那条拷贝 | `dsi_ext_fb_get()` + `dsi_ext_fb_sync()` |
| 读卡死后的恢复 | 无（只能断电） | `dsi_ext_link_reset()` + 重新 bringup |

---

## 安装

**方式一：组件注册表（推荐）**

```yaml
dependencies:
  martlet-tech/esp_dsi_ext: "^0.2.0"
```

**方式二：git 依赖**（想跟主分支走，或注册表访问不了时）

```yaml
dependencies:
  martlet-tech/esp_dsi_ext:
    git: https://github.com/Martlet-Tech/esp-dsi-ext.git
```

**方式三：本地路径**

```yaml
dependencies:
  martlet-tech/esp_dsi_ext:
    path: ../../esp-dsi-ext
```

**方式四：直接放工程里** —— 把本仓库放到工程的 `components/esp_dsi_ext/` 下，
main 里 `REQUIRES esp_dsi_ext` 即可。

> 只对 `SOC_MIPI_DSI_SUPPORTED` 的芯片有效（ESP32-P4 及同族）。其它 target 会编译成
> 返回 `ESP_ERR_NOT_SUPPORTED` 的 stub，可以安全地放在多目标工程里。

---

## 例程

两个都能单独拷出去 `idf.py build`（自包含，不依赖仓库里的其他文件）。

| 例程 | 用途 |
|---|---|
| [`examples/bringup_minimal`](examples/bringup_minimal) | **点亮一块新屏的最小骨架。** 不接 LVGL，只有 screen 一层：上电 / 复位 / DSI / 初始化表 / video mode / 纯色轮播。三个文件改完就是你的屏。 |
| [`examples/incell_540x810_2lane`](examples/incell_540x810_2lane) | **完整参考。** 显示 + 触摸 + LVGL music demo，含一套实测可用的参数（porch / lane / DPI / LDO）。 |

两个例程共用同一套骨架，这个骨架也是推荐的项目组织方式：

```
main/
├── board_config.h     引脚 / 电源 / 背光 / 触摸引脚      ← 换板子只改这里
├── panel_timing.h     分辨率 / porch / lane / DPI / 格式  ← 唯一事实源
├── panel_init.c/h     初始化命令表（生成物，勿手改）
├── screen.c/h         显示：init / backlight / fill / vpg / panel
├── touch.c/h          触摸：init / touch_read / stats
├── lv_port.c/h        LVGL：display / indev / tick / 任务
└── app_main.c         只做组装
```

约束：

- `screen.h` 和 `touch.h` **都不 include `lvgl.h`**，彼此也不互相 include
- 只有 `lv_port` 同时认识两边 —— 它是胶水层，这是设计
- `app_main` 只按顺序调用上面几个模块，不碰任何 hal / 寄存器头
- 引脚只在 `board_config.h`，时序只在 `panel_timing.h` —— 别在 `screen.c` 里写死数字

---

## 用法

### 1. 初始化 + 发任意 DT 的命令

```c
#include "dsi_ext.h"

// esp_lcd_new_dsi_bus() / esp_lcd_new_panel_dpi() 之后:
ESP_ERROR_CHECK(dsi_ext_init(0));                                 // 取 host / bridge 句柄
ESP_ERROR_CHECK(dsi_ext_cmd_channel_config(false, false, true));  // 关 cmd_ack, 写命令走 LP

uint8_t cmd_0A[] = { 0x0A, 0x01 };          // 例如: 厂商自定义寄存器
ESP_ERROR_CHECK(dsi_ext_tx(0, 0x39, cmd_0A, sizeof(cmd_0A)));
```

`dsi_ext_tx(vc, dt, data, len)`：`dt = 0x05/0x15/0x39/0x29` 是 DCS 短写/长写，
`0x03/0x13` 是通用短写；payload 自动按 4 字节对齐压 FIFO（DWC 要求按字写）。

### 2. 从面板读

```c
uint8_t v = 0;
uint16_t n = 0;

ESP_ERROR_CHECK(dsi_ext_set_read_speed(true));      // ★ 读请求走 LP（多数屏必须）
ESP_ERROR_CHECK(dsi_ext_set_max_read_time(0x8000)); // 读命令硬件看门狗

esp_err_t err = dsi_ext_rx_dcs(0x0A, &v, 1, 100, &n);   // 读 RDDPM, 100ms 超时
if (err == ESP_OK) {
    ESP_LOGI(TAG, "0x0A = 0x%02X (%u 字节)", v, n);
}
```

要自己指定 DT：

```c
uint8_t req[1] = { 0x0A };
dsi_ext_rx(0, 0x04, req, 1, &v, 1, 100, &n);   // Generic Read Request
```

- `req_len`（0/1/2）决定请求包 header 宽度 —— 读请求在 MIPI 里**永远是短包**
- `read_len` 决定发给面板的 `0x37 SET_MAXIMUM_RETURN_PKT` 和本地缓冲上限
- 进入时关 video mode、开 BTA，退出时**恢复原来的 video mode 状态**
- 返回实际读到的字节数（`actual_len`），不是恒 `ESP_OK`

### 3. 开机自检图案（VPG）

```c
dsi_ext_vpg(panel, DSI_EXT_PATTERN_BAR_VERTICAL);   // host 内部生成，不经过 framebuffer
vTaskDelay(pdMS_TO_TICKS(1000));
dsi_ext_vpg(panel, DSI_EXT_PATTERN_OFF);            // 必须关，否则 draw_bitmap 不上屏
```

图案由 DSI host 内部产生，**不经过 framebuffer、也不经过 bridge 的 DPI 输入通路**，
是划分故障域最快的手段：图案正常 → 链路/PHY/面板/时序全对，问题在数据通路。

### 4. 直写面板内部 framebuffer

```c
uint8_t *fb = NULL;
dsi_ext_fb_get(panel, &fb);                 // 面板内部 framebuffer
fill(fb, 540 * 810 * 3);                    // 自己按 B,G,R 写（TRM 表 43.4-1）
dsi_ext_fb_sync(fb, 540 * 810 * 3);         // PSRAM 带 cache，必须写回
```

绕开 `draw_bitmap` 里那条拷贝，用来区分"拷贝错"还是"时序错"。

### 5. IDF 写死、但硬件支持的行为

```c
dsi_ext_set_clock_lane(true);                                  // 消隐期不回 LP，全程 HS
dsi_ext_set_timing_polarity(true, false, false, false, false);  // hsync 拉低
dsi_ext_set_pixel_type(0, 1);                                   // raw_type + dpi_config
```

改完可以回读寄存器确认，比如 `host->lpclk_ctrl.auto_clklane_ctrl`。

### 6. 读失败后的恢复

```c
esp_lcd_panel_del(panel);
dsi_ext_link_reset();          // host/PHY shutdownz 拉低→放开，复位数逻辑
esp_lcd_del_dsi_bus(bus);
// 然后重新 new_dsi_bus / new_panel_dpi / 重发初始化表
```

---

## 实测结论（ESP32-P4 + 540×810 incell TDDI）

### 读请求必须跟着写命令一起走 LP

这是本组件最值得记的一条：

- `DSI_HOST_CMD_MODE_CFG (0x0068)` 里，**读类数据包有独立的速度位**：
  `bit19 DCS_SR_0P_TX`、`bit12~14 GEN_SR_xP_TX`、`bit24 MAX_RD_PKT_SIZE`，
  复位值全是 **0 = HS**。
- IDF 只设置写方向的速度位，读方向从来不管。
- 如果面板只实现了 LP（LPDT）命令入口 —— 很多便宜的 TDDI 都是这样 —— 那么读请求
  按 HS 发出去它**根本听不见**，自然不回 BTA。
- 后果不是"读不到"，而是：`gen_rd_cmd_busy` 永久为 1 → 读包永久占住命令 FIFO →
  **之后所有写命令都超时，画面直接黑掉，且软件无法恢复**。

### 几个不能靠超时救的坑

| 现象 | 真相 |
|---|---|
| 读超时后整条链路死掉 | `gen_rd_cmd_busy` 只读，只有读事务真正完成才会清；IDF 的 `mipi_dsi_ll_reset_register()` 只复位 `reg_rst_en_dsi_brg`（bridge），**不复位 host** |
| 设了 `set_timeout_count()` 也不管用 | `TO_CNT_CFG`(0x0078) 那几个是**竞争检测**计数器，TRM 原文写着"不会产生中断"，不是读看门狗 |
| 真正的读看门狗 | `DSI_HOST_PHY_TMR_RD_CFG (0x00F4) MAX_RD_TIME`，IDF 给 6000（≈60µs @100MHz byteclk），太紧 |
| 读不能在初始化表发到一半时做 | 读挂了，剩下半张表全丢，屏必黑 |

### 其它

- `esp_lcd_panel_dpi_config.flags.use_dma2d` 在 5.5.1 **不支持**，实际是 CPU memcpy，
  所以"直写内部 fb"和"走 draw_bitmap"结果一样时，不能据此判断拷贝环节有问题。
- `draw_bitmap` 的 end 参数是排他的，`area->x2 + 1` 才对。
- LVGL 9 的 `lv_color_t` 是 `{blue, green, red}`，与 TRM 表 43.4-1 的 `B0 G0 R0` 一致，
  **不要**在 flush 里交换 R/B（会造成整屏反色）。

---

## 依赖与限制

- 依赖 IDF **私有头文件**（`hal/mipi_dsi_host_ll.h` 等）。已在 **v5.5.1** 上实测；
  升级 IDF 后需要复查能否编译（签名/字段改名是可能的）。
- `PRIV_REQUIRES hal soc` 等于在构建系统里明示"我在用私有接口"。
- 只能改**寄存器可配**的东西。真正缺硬件支持的（比如 24-bit 在 2 lane 上的位拆分方式）
  仍然改不了。
- 目前只在 ESP32-P4 上验证过。

## License

Apache-2.0
