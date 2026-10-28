/*
 * 面板时序 —— **唯一事实源**。
 *
 * 本文件对应一块 540x810 的 incell TDDI（触摸与显示同一颗 die），
 * 2 lane / RGB888 / burst video mode。
 *
 * 改分辨率、改 porch、改 lane 速率，只改这个文件。
 */
#pragma once

/* ---------------- 有效分辨率和 porch ---------------- */

#define PANEL_H_SIZE        540
#define PANEL_V_SIZE        810

#define PANEL_HSW           10      /* hsync pulse width   */
#define PANEL_HBP           120     /* hsync back porch    */
#define PANEL_HFP           120     /* hsync front porch   */

#define PANEL_VSW           13      /* vsync pulse width   */
#define PANEL_VBP           6       /* vsync back porch  ★ */
#define PANEL_VFP           110     /* vsync front porch   */

/* ★★★ VBP 是这块屏的颜色开关 ★★★
 *
 * VBP = 8 时：纯红显示成青、绿显示成粉、蓝显示成黄，而白/黑正常。
 *            八组观测（含 VPG 的八条彩条）符合同一条规律
 *              R_out=(b1+b2)/2, G_out=(b0+b2)/2, B_out=(b0+b1)/2
 *            也就是每个输出通道拿到另外两个源字节各一半（位级交织）。
 * VBP = 6 时：全部正常。
 *
 * 这一条改了十几轮才找到 —— 期间把颜色链路能拆的都拆了：
 *   in_color_format / pixel_type.raw_type / dpi_config 四档 / rgb_ele_order /
 *   同步极性 / clock lane HS·AUTO / burst·non-burst / trunks / null packet /
 *   DMA2D 拷贝 vs 直写内部 framebuffer —— 全部无反应。
 *
 * 教训：porch 不是"留余量"，它是面板接收状态的一部分。
 *      而且 VPG 也过 host 的打包器，所以"VPG 正常"证明不了颜色对。
 */

/* ---------------- DSI 链路 ---------------- */

#define PANEL_LANE_NUM      2
#define PANEL_LANE_MBPS     800

/* DPI 像素时钟 (MHz)。P4 的 DPI 只能从 240MHz 整数分频，
 * **只有 240 的约数才准确**：24 / 30 / 40 / 48 / 60。
 * 写 32 会被截断成 240/7 = 34.29MHz，帧率和时序都会偏。 */
#define PANEL_DPI_CLK_MHZ   40

/* ---------------- 像素格式 ---------------- */

/* 24 = RGB888。内存排列是 B,G,R（TRM 43.4.1），不是 R,G,B。 */
#define PANEL_BITS_PER_PIXEL    24

/* ---------------- 同步极性 ---------------- */

/* 全 0 = active-high，与 IDF 官方 mipi_dsi 例程一致。
 * 这块屏在 active-high 下几何正确；极性错的表现是图像整体位移、
 * 露黑边/白边，与颜色无关。 */
#define PANEL_HSYNC_LOW     0
#define PANEL_VSYNC_LOW     0
#define PANEL_DE_LOW        0

/* ---------------- 派生值（不要手改） ---------------- */

#define PANEL_HTOTAL        (PANEL_H_SIZE + PANEL_HSW + PANEL_HBP + PANEL_HFP)   /* 790 */
#define PANEL_VTOTAL        (PANEL_V_SIZE + PANEL_VSW + PANEL_VBP + PANEL_VFP)   /* 939 */

/* 帧率 = 40MHz / (790 * 939) = 53.9Hz，行周期 = 19.75us */
