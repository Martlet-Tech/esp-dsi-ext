# Changelog

## 0.3.0 — 未发布

### ⚠ 破坏性变更：`dsi_ext_cmd_channel_config()` 第三个参数改成枚举

**这是一个真实踩坑导致的修复。** 旧签名：

```c
esp_err_t dsi_ext_cmd_channel_config(bool cmd_ack, bool te_ack, bool all_lp);
```

头文件写的 `all_lp = false` 是"保持 IDF 默认"，但实现里 `false` 走的是
**把写命令全部改成 HS** 的分支。两者不一致，而且 `bool` 表达不了三态。

**踩坑现场**：YTS295KLAB 做结构归一化时，把直接调 hal 的
`mipi_dsi_host_ll_enable_cmd_ack(host, false)` 换成组件调用，按文档传了
`dsi_ext_cmd_channel_config(false, false, false)`。写命令从 LP 翻成 HS，
而这块屏只认 LP 命令入口：

```
面板收不到 HS 命令 → 不回 BTA → gen_rd_cmd_busy 卡住
→ 命令 FIFO 永久满 → cmd #19 0x11 FAILED: ESP_ERR_TIMEOUT → 屏不亮
```

前 18 条"成功"是假象：`dsi_ext_tx()` 只要 header 进了 FIFO 就返回 OK，
它不检查面板有没有收到。

**新签名**：

```c
typedef enum {
    DSI_EXT_WR_SPEED_KEEP = 0,   /* 一个都不改，保持调用前的值 */
    DSI_EXT_WR_SPEED_LP,         /* 全部写命令走 LP（= IDF 默认） */
    DSI_EXT_WR_SPEED_HS,         /* 全部写命令走 HS */
} dsi_ext_wr_speed_t;

esp_err_t dsi_ext_cmd_channel_config(bool cmd_ack, bool te_ack,
                                     dsi_ext_wr_speed_t wr_speed);
```

迁移：

| 旧写法 | 新写法 | 说明 |
|---|---|---|
| `(false, false, true)` | `(false, false, DSI_EXT_WR_SPEED_LP)` | 行为不变 |
| `(false, false, false)` | `(false, false, DSI_EXT_WR_SPEED_KEEP)` | **行为变了**——旧版这里会改成 HS，新版不改。这正是修复的目的 |
| 只想关 ACK | `dsi_ext_disable_cmd_ack()` | 新函数，见下 |

### 新增：`dsi_ext_disable_cmd_ack()`

```c
esp_err_t dsi_ext_disable_cmd_ack(void);
```

等价于 `dsi_ext_cmd_channel_config(false, false, DSI_EXT_WR_SPEED_KEEP)`。
**这是绝大多数工程真正需要的那个函数**——归一化时"把直接调 hal 的
`enable_cmd_ack(host, false)` 换成组件调用"这个动作，语义上只该关 ACK，
不该顺手动写速度。用它就没有"第三个参数该传什么"的歧义。

### 新增：video mode 的 LP 行为控制

补齐原厂例程（430/546）里靠 `components/dsi_ll` 私有封装做的两件事。
这两个能力 0.1.0 / 0.2.0 都缺，导致那些工程没法把 `dsi_ll` 换成本组件。

- `dsi_ext_disable_lp_all()` — 关掉行消隐（HBP/HFP）、场消隐
  （VSYNC/VBP/VFP/VACT）、命令通道三段的 LP 回退。等价于
  `dsi_ll_dpi_disable_lp()`。部分面板要求 HS clock 全程连续，每次回 LP
  都会重新锁定，表现为抖动/斜线/闪烁。
- `dsi_ext_enable_lp_all()` — 恢复 IDF 默认（各段都允许回 LP）。
- `dsi_ext_disable_frame_ack()` — 关掉帧末 BTA ack。等价于
  `dsi_ll_dpi_disable_frame_ack()`。注意关掉后 `on_refresh_done` 回调不再触发。

三个都**必须在 `esp_lcd_panel_init()` 之后**调用，它内部会重设这几段。

注意 `dsi_ext_disable_lp_all()` 不含 clock lane 那一位
（`auto_clklane_ctrl`），与 `dsi_ll_dpi_disable_lp()` 的行为一致；
需要 clock lane 全程 HS 时另调 `dsi_ext_set_clock_lane(true)`。

### 已知未覆盖

- `dsi_ll_set_video_burst_type()` 仍无对应 API。当前使用该组件的工程
  （YTS430KLAK / YTS546XLAB）都没有调用它，暂不需要。

### 破坏性变更：两个例程的复位接口

`examples/` 里的复位从模块内部的 `static` 函数改成公开接口，**调用点移到 `app_main`**：

| 旧 | 新 |
|---|---|
| `screen.c` 内部 `static board_reset()`，由 `screen_init()` 调用 | `screen_reset()`，由 `app_main` 调用 |
| `touch.c` 内部 `static hw_reset()`，由 `touch_init()` 调用 | `touch_reset()`，由 `app_main` 调用（incell 例程） |

理由：复位时机和顺序是板级/IC 级的事，写在 `app_main` 里一眼可见；调试时能单独重放；
换 IC 只改编排不动模块。`screen_init()` / `touch_init()` 现在**不含**复位。

这是例程的示范形态，不是对所有工程的强制要求 —— 已有工程做结构归一化时，
只需让复位对外可见，**不必搬调用点**（见 Martlet-Tech/skills 配套的结构指南）。

## 0.2.0 — 2026-10-28

### 新增：两个例程

- `examples/bringup_minimal` — 点亮一块新屏的最小骨架。不接 LVGL，
  只有 `screen` 一层：上电/复位/DSI/初始化表/video mode/纯色轮播。
  三个文件改完就是你的屏（`board_config.h` / `panel_timing.h` / `panel_init.c`）。
- `examples/incell_540x810_2lane` — 完整参考：显示 + 触摸 + LVGL music demo。
  演示分层写法（`screen` / `touch` / `lv_port` 互不知道对方，只有 `lv_port` 是胶水），
  以及一组实测可用的参数（porch / lane / DPI / LDO）。

两个例程都自包含，可以单独拷出去 `idf.py build`。

### 组件本体

无接口变更，功能与 0.1.0 相同。

## 0.1.0 — 2026-10-28

首个版本。全部能力都不修改 ESP-IDF，只在自己的编译单元里包含 `hal` 的私有 inline 接口。

### 命令发送
- `dsi_ext_init(port)` — 取 DSI host / bridge 句柄
- `dsi_ext_tx(vc, dt, data, len)` — 任意数据类型的命令发送（0x03/05/13/15/29/39），
  payload 按 4 字节对齐压 FIFO；命令/写 FIFO 等待带次数上限，超时返回 `ESP_ERR_TIMEOUT`
- `dsi_ext_cmd_channel_config(cmd_ack, te_ack, all_lp)` — 关掉 IDF 默认打开的 cmd_ack，
  并逐类设置写命令的 LP/HS 速度

### 读
- `dsi_ext_rx(vc, dt, req, req_len, buf, read_len, timeout_ms, actual_len)` — 任意 DT 读取，
  全程带超时，返回实际读到的字节数，退出时恢复原 video mode 状态
- `dsi_ext_rx_dcs(cmd, buf, read_len, timeout_ms, actual_len)` — DCS 读便捷封装
- `dsi_ext_set_read_speed(lp)` — 设置**读请求本身**的传输速度（`DCS_SR_0P_TX` 等），
  IDF 与多数示例都漏了这一位；面板只收 LP 命令时，这正是读不通的原因
- `dsi_ext_set_max_read_time(cycles)` — `MAX_RD_TIME`（TRM 43.79）
- `dsi_ext_enable_bta_timeout(bta, lp_rx)` — 打开 BTA / hs_rd / lp_rd 超时
- `dsi_ext_rx_faulted()` / `dsi_ext_rx_clear_fault()` — 读超时后的故障锁存
- `dsi_ext_link_reset()` — host/PHY `shutdownz` 掉电复位，用于读卡死后重建链路

### 图案与 framebuffer
- `dsi_ext_vpg(panel, pattern)` — OFF / 垂直彩条 / 水平彩条 / BER（host 内部生成）
- `dsi_ext_fb_get(panel, &fb)` / `dsi_ext_fb_sync(fb, size)` — 直写面板内部 framebuffer

### IDF 写死的硬件行为
- `dsi_ext_set_clock_lane(force_hs)` — 消隐期是否回 LP
- `dsi_ext_set_timing_polarity(hsync, vsync, de, shutdn, colorm)` — 5 个同步极性
- `dsi_ext_set_pixel_type(raw_type, dpi_config)` — bridge `pixel_type` 直写

### 平台
- 仅 `SOC_MIPI_DSI_SUPPORTED`；其它 target 编译为返回 `ESP_ERR_NOT_SUPPORTED` 的 stub
- 实测环境：ESP32-P4（2 lane @800Mbps，540×810 RGB888）
