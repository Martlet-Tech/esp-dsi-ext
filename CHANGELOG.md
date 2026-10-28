# Changelog

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
