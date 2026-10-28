/*
 * 面板时序 —— **唯一事实源**。
 *
 * 改分辨率、改 porch、改 lane 速率，只改这个文件，别的地方一律引用这里的宏。
 * 如果发现自己在别处又写了一遍 540 或 800 这种数字，那是 bug。
 *
 * ★ porch 不要猜。HSW/HBP/HFP/VSW/VBP/VFP 六个值必须来自面板规格书或供应商给的
 *   参考配置。VBP/VFP 尤其敏感：它们不只是"留余量"，还决定面板的接收状态。
 *   本仓库另一个例程（incell_540x810_2lane）就是 VBP 差 2 行导致整屏偏色，
 *   而软件层所有颜色相关的旋钮都试遍了才在 FAE 提示下改对。
 */
#pragma once

/* ---------------- 有效分辨率和 porch ---------------- */

#define PANEL_H_SIZE        480
#define PANEL_V_SIZE        800

#define PANEL_HSW           10      /* hsync pulse width   */
#define PANEL_HBP           60      /* hsync back porch    */
#define PANEL_HFP           60      /* hsync front porch   */

#define PANEL_VSW           10      /* vsync pulse width   */
#define PANEL_VBP           10      /* vsync back porch  ★ */
#define PANEL_VFP           20      /* vsync front porch ★ */

/* ---------------- DSI 链路 ---------------- */

#define PANEL_LANE_NUM      2
#define PANEL_LANE_MBPS     500

/* DPI 像素时钟 (MHz)。
 * P4 的 DPI 只能从 240MHz 整数分频，所以**只有 240 的约数才准确**：
 *   24, 30, 40, 48, 60  (对应分频 10, 8, 6, 5, 4)
 * 写 32 这类非约数会被截断成 240/7=34.29，帧率随之偏。
 *
 * 校验关系：lane_bitrate >= dpi_clk * 8 * (24 / bits_per_pixel)
 * 对 RGB888 即 lane_total >= dpi_clk * 8。上面 2 x 500 = 1000 >= 240，够。 */
#define PANEL_DPI_CLK_MHZ   30

/* ---------------- 像素格式 ---------------- */

/* RGB888 / RGB565 / RGB666。改这里要同时改 screen.c 里的 in_color_format，
 * 所以 screen.c 是引用本宏来填的，不需要两处改。 */
#define PANEL_BITS_PER_PIXEL    24

/* ---------------- 同步极性 ---------------- */

/* 1 = active-low。IDF 默认全 active-high。
 * 极性错表现为图像整体位移、露黑边/白边、或行边界错乱 —— 不影响颜色。 */
#define PANEL_HSYNC_LOW     0
#define PANEL_VSYNC_LOW     0
#define PANEL_DE_LOW        0

/* ---------------- 派生值（不要手改） ---------------- */

#define PANEL_HTOTAL        (PANEL_H_SIZE + PANEL_HSW + PANEL_HBP + PANEL_HFP)
#define PANEL_VTOTAL        (PANEL_V_SIZE + PANEL_VSW + PANEL_VBP + PANEL_VFP)

/* 行周期 = HTOTAL / DPI_CLK，帧率 = DPI_CLK / (HTOTAL * VTOTAL)。
 * 打开 screen.c 里的日志就能看到实际值，别在这里算。 */
