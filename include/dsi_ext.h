/*
 * dsi_ext —— 给 ESP-IDF 没暴露成 API 的 DSI host / bridge 能力开一扇门
 *
 * 背景:
 *   esp_lcd 的 DSI 支持只覆盖"正常路径": DPI 面板 + DCS 命令 + 固定极性。
 *   但真实面板经常需要更多:
 *     - 初始化命令必须走 Generic 长包, 而 esp_lcd_panel_io_tx_param() 把 DT 写死成 DCS
 *     - 面板不回 ACK 会让 IDF 的裸 while 死等, 触发 task_wdt
 *     - 同步极性 / clock lane 状态 / 像素排布 IDF 全写死, 没有 API 可改
 *
 *   这些能力在 IDF 里都存在, 只是躺在 hal 层的 static inline 函数里
 *   (mipi_dsi_host_ll_* / mipi_dsi_brg_ll_*), 没有对上层暴露。
 *
 * 本组件的做法:
 *   不修改 IDF 源码, 只在自己的编译单元里包含 hal 的私有头文件, 直接调用这些
 *   inline 函数 / 读写寄存器结构体。代价是依赖 IDF 内部头文件, 升级 IDF 时
 *   需要复查本文件。
 *
 * 目标平台:
 *   仅 SOC_MIPI_DSI_SUPPORTED 的芯片 (esp32p4 及同族)。其它 target 编译时
 *   所有接口退化成返回 ESP_ERR_NOT_SUPPORTED 的 stub, 可以安全地在多目标
 *   工程里被依赖。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 生命周期 ---------------- */

/**
 * @brief 绑定某个 DSI 端口上的 host / bridge 句柄
 *
 * 必须在 esp_lcd_new_dsi_bus() 之后、任何 dsi_ext_* 调用之前执行。
 * 内部只保存指针, 不改变任何硬件状态。
 *
 * @param port DSI 端口号, 目前 esp32p4 只有 0
 */
esp_err_t dsi_ext_init(int port);

/**
 * @brief 取底层 host 寄存器基址 (高级逃生口)
 *
 * 返回的是 dsi_host_dev_t*, 供调用方自己调 hal 里没被本组件封装的接口。
 * 不想依赖内部头文件的话不要用这个函数。
 */
void *dsi_ext_host(void);

/* ---------------- 命令通道 ---------------- */

/**
 * @brief 写命令的传输速度 (对应 DSI_HOST_CMD_MODE_CFG 里各类写数据包的 LP/HS 位)
 *
 * ★ 这个枚举存在的唯一原因, 是旧版 `dsi_ext_cmd_channel_config()` 的第三个参数
 *   是 bool `all_lp`, 文档写的是 "false = 保持 IDF 默认", 但实现里 false 走的是
 *   **改成 HS** 的分支。也就是说文档和实现不一致, 而且 bool 表达不了三态:
 *
 *       想做的事                      | 旧 bool 参数能表达吗
 *       ------------------------------|---------------------
 *       保持 IDF 默认 (全 LP)         | true (但语义不明)
 *       全部改成 HS                   | false (但文档说是"保持默认")
 *       只改其中某一类                | 不能
 *
 *   实际踩坑记录: YTS295KLAB 归一化时按文档传了 false, 写命令从 LP 翻成 HS,
 *   面板只认 LP 命令入口 -> 收不到 -> 不回 BTA -> gen_rd_cmd_busy 卡住 ->
 *   命令 FIFO 永久满 -> `cmd #19 0x11 FAILED: ESP_ERR_TIMEOUT`。
 *   前 18 条"成功"是假象: dsi_ext_tx() 只要 header 进了 FIFO 就返回 OK。
 *
 * @note IDF 在 `esp_lcd_new_panel_io_dbi()` 里把 DCS/Gen 的写命令**全部设成 LP**
 *       (`esp_lcd_panel_io_dbi.c:46-57`)。所以"保持默认" == 全 LP。
 */
typedef enum {
	/**
	 * 保持调用前的值, 一个都不改。
	 *
	 * 这是最安全的默认: 只想要 `cmd_ack` 行为、不想碰速度时用它。
	 */
	DSI_EXT_WR_SPEED_KEEP = 0,

	/** 全部写命令 (DCS short 0/1, DCS long, Gen short 0/1/2, Gen long) 走 LP */
	DSI_EXT_WR_SPEED_LP,

	/** 全部写命令走 HS */
	DSI_EXT_WR_SPEED_HS,
} dsi_ext_wr_speed_t;

/**
 * @brief 配置命令通道: ACK 行为 + 各类写的传输速度
 *
 * IDF 的 esp_lcd_new_panel_io_dbi() 会在这里打开 cmd_ack, 而 bus 层把超时全
 * 置 0 —— 面板不回 ACK 就永久等总线转向。多数屏不受影响, 但有些屏(比如
 * 只认 Generic 包的 TDDI)会直接卡死, 表现为 task_wdt。
 *
 * @param cmd_ack 命令是否要求 DSI 设备的 ACK
 * @param te_ack  TE 是否要求 ACK
 * @param wr_speed 写命令速度, 见 @ref dsi_ext_wr_speed_t
 *
 * @note 只想关 cmd_ack、不碰速度时传 `DSI_EXT_WR_SPEED_KEEP`。
 *       想与 IDF 默认保持一致时传 `DSI_EXT_WR_SPEED_LP` (两者当前效果相同,
 *       但语义不同: 前者依赖"调用前是什么", 后者是显式写死)。
 *
 * @code
 * // 只关 cmd_ack, 速度保持 IDF 设好的 LP
 * dsi_ext_cmd_channel_config(false, false, DSI_EXT_WR_SPEED_KEEP);
 *
 * // 显式要求全 LP (与 IDF 默认一致, 幂等)
 * dsi_ext_cmd_channel_config(false, false, DSI_EXT_WR_SPEED_LP);
 * @endcode
 */
esp_err_t dsi_ext_cmd_channel_config(bool cmd_ack, bool te_ack,
				     dsi_ext_wr_speed_t wr_speed);

/**
 * @brief 只关命令 ACK, 写速度一点不碰
 *
 * 等价于 `dsi_ext_cmd_channel_config(false, false, DSI_EXT_WR_SPEED_KEEP)`。
 *
 * **这是绝大多数工程真正需要的那个函数。** 归一化时"把直接调 hal 的
 * `mipi_dsi_host_ll_enable_cmd_ack(host, false)` 换成组件调用"这个动作,
 * 语义上只该关 ACK —— 一旦顺手动了写速度, 就是行为改动, 可能直接导致点不亮。
 * 用这个函数就没有"第三个参数该传什么"的歧义。
 */
esp_err_t dsi_ext_disable_cmd_ack(void);

/**
 * @brief 发送一条任意数据类型的 DSI 命令
 *
 * 与 IDF 的 mipi_dsi_hal_host_gen_write_dcs_command() 逻辑一致, 唯一区别是
 * DT 由调用方指定, 可以发 Generic 包 (0x03/0x13/0x23/0x29) 而不只是 DCS。
 *
 *   dt = 0x39 / 0x29 : 长包, payload = data[0..len-1] (data[0] 就是寄存器/命令)
 *   dt = 0x05 / 0x03 : 短包, data[0] 放进包头
 *   dt = 0x15 / 0x13 : 短包, data[0] + data[1] 放进包头
 *
 * FIFO 等待带超时, 面板不接收时返回 ESP_ERR_TIMEOUT 而不是死等。
 *
 * @param vc   虚拟通道, 一般为 0
 * @param dt   MIPI DSI 数据类型
 * @param data 命令 + 参数, data[0] 必须是命令本身
 * @param len  data 的有效字节数
 */
esp_err_t dsi_ext_tx(uint8_t vc, uint8_t dt, const uint8_t *data, uint16_t len);

/* ---------------- 读: 任意 DT, 带超时 ---------------- */

/**
 * @brief 从面板读回数据 (任意 DT)
 *
 * IDF 的 esp_lcd_panel_io_rx_param() 只发 DCS Read (0x06), 而且底层三处
 * 都是裸 while 没有超时, 失败也恒返回 ESP_OK, 读完还不恢复 video mode。
 * 本函数把这几件事都做干净了:
 *
 *   1) DT 由调用方指定: 0x06 DCS Read / 0x04 Generic Read(无参)
 *      0x14 Generic Read(1 参) / 0x24 Generic Read(2 参)
 *   2) 请求包的长度由 req_len 自动决定 (0/1/2)。读请求在 MIPI 里永远是
 *      短包, 所以不需要"长包/短包"的开关; read_len 决定的是下面两件事:
 *        - 发 0x37 SET_MAXIMUM_RETURN_PKT 告诉面板最多回多少字节
 *        - 本地缓冲的容量, 面板回多了按 read_len 截断
 *   3) 全过程带超时, 卡住返回 ESP_ERR_TIMEOUT, 不会打 task_wdt
 *   4) 返回实际读到的字节数 (*actual_len), 而不是恒 ESP_OK
 *   5) 退出时恢复调用前的 video mode 状态 (IDF 关掉就不管了, 画面会停)
 *
 * ★ 严重警告: 读请求发出后, 面板必须回 BTA, 否则 host 的 gen_rd_cmd_busy 永远
 *   为 1 —— 那个读包就一直占着命令 FIFO, **此后所有写命令都会 ESP_ERR_TIMEOUT,
 *   且无法恢复**(该状态位只读, IDF 没有清它的接口)。
 *   所以:
 *     - 只能在面板已经能正常应答的时候读 (至少 11h 发完之后)
 *     - **绝对不要在初始化表发到一半时读**, 读挂了剩下的初始化命令就全丢了,
 *       屏幕会不亮。要读就等整张表发完再读。
 *   为了不让错误重复发生, 一次超时之后本组件会锁存故障, 后续 dsi_ext_rx()
 *   直接返回 ESP_ERR_INVALID_STATE (可用 dsi_ext_rx_clear_fault() 解锁)。
 *
 * @param vc        虚拟通道, 一般 0
 * @param dt        读请求的数据类型
 * @param req       请求里的参数字节 (0x06 时就是命令本身), req_len=0 可传 NULL
 * @param req_len   参数字节数, 0~2
 * @param buf       接收缓冲
 * @param read_len  期望读回的字节数 (上限)
 * @param timeout_ms 等"命令发完"和"数据到达"的超时, 单位 ms
 * @param actual_len 输出: 实际读到的字节数, 可为 NULL
 */
esp_err_t dsi_ext_rx(uint8_t vc, uint8_t dt, const uint8_t *req, uint8_t req_len,
		     void *buf, uint16_t read_len, uint32_t timeout_ms,
		     uint16_t *actual_len);

/**
 * @brief dsi_ext_rx() 的便捷封装: 读 DCS 寄存器 (DT = 0x06)
 *
 * 例: 读 0x0A (RDDPM, 显示电源模式)
 *     uint8_t v; uint16_t n;
 *     dsi_ext_rx_dcs(0x0A, &v, 1, 100, &n);
 */
esp_err_t dsi_ext_rx_dcs(uint8_t cmd, void *buf, uint16_t read_len,
			 uint32_t timeout_ms, uint16_t *actual_len);

/**
 * @brief 打开 DSI host 内部的 BTA / LP-RX 超时
 *
 * ★ IDF 在 esp_lcd_mipi_dsi_bus.c:118 把 7 个超时全部写成 0, 注释是
 *   "wait indefinitely for a read response from the DSI device" —— 也就是说
 *   面板不回 BTA 时, host 会永远卡在读事务上, 读包永久占住命令 FIFO, 之后
 *   所有写命令全部超时, 且软件无法恢复。
 *
 *   打开这个超时之后, 硬件自己会在 BTA 阶段放弃, gen_rd_cmd_busy 随之清零,
 *   命令 FIFO 解锁 —— 读失败就只是读失败, 不会拖死整条链路。
 *
 * dsi_ext_rx() 会自己确保至少有一个默认 BTA 超时, 一般不需要手动调用。
 *
 * @param bta_cycles   BTA 超时, 单位 = lane byte clock 周期 (800Mbps 时约 10ns/cycle)
 *                     0 = 保持 IDF 的禁用状态
 * @param lp_rx_cycles LP 接收超时, 单位 = TO 时钟周期; 0 = 禁用
 */
esp_err_t dsi_ext_enable_bta_timeout(uint16_t bta_cycles, uint16_t lp_rx_cycles);

/**
 * @brief 设置"读请求"本身的传输速度 (HS / LP)
 *
 * ★ 这是 IDF 和本组件之前都漏掉的一个开关。TRM 43.7.2 的
 *   DSI_HOST_CMD_MODE_CFG (0x0068) 里, 读类数据包有独立的速度位:
 *     bit19 DCS_SR_0P_TX   DCS 短读
 *     bit12~14 GEN_SR_0P/1P/2P_TX   通用短读
 *     bit24 MAX_RD_PKT_SIZE          0x37 SET_MAXIMUM_RETURN_PKT
 *   全部复位值是 0 = HS。也就是说: 即使写命令配成 LP, 读请求默认仍走 HS。
 *
 *   如果面板只实现了 LP(逃逸模式 LPDT) 命令入口 —— 这块屏所有写命令都是
 *   LP 才能收 —— 那么按默认的 HS 发读请求, 面板根本听不见, 自然不回 BTA,
 *   于是 gen_rd_cmd_busy 永远不清。这是目前唯一还没验证过的变量。
 *
 * @param lp true = 读请求走 LP (与写命令一致), false = 走 HS (IDF 默认)
 */
esp_err_t dsi_ext_set_read_speed(bool lp);

/**
 * @brief 设置读命令的硬件超时 (TRM 43.79 DSI_HOST_PHY_TMR_RD_CFG 0x00F4)
 *
 * TRM 原话: "配置执行读命令所需的最大时间（以通道字节时钟周期为单位）,
 * 此寄存器只能在没有任何读命令进行时修改。"
 * IDF 在 esp_lcd_mipi_dsi_bus.c:120 设的是 6000 (800Mbps 下约 60us)。
 *
 * @param byte_clocks 超时周期数, 0 = 不设
 */
esp_err_t dsi_ext_set_max_read_time(uint32_t byte_clocks);

/**
 * @brief 把 DSI host + PHY 数字逻辑整体复位一次 (读卡死后的恢复用)
 *
 * 读卡死后 gen_rd_cmd_busy 清不掉, 但它属于数字逻辑状态 —— TRM 43.4.3.1.2:
 * "关闭状态会禁用所有模拟模块, 重置所有数字逻辑"。所以:
 *   host pwr_up.shutdownz = 0 -> 1ms -> 1, PHY shutdownz 0 -> 1, 再 phy reset
 * 之后必须重新走一遍 esp_lcd_new_dsi_bus() / new_panel_dpi() / 初始化表,
 * 因为这些寄存器都回到默认值了。
 *
 * 调用时机: esp_lcd_panel_del() 之后、esp_lcd_del_dsi_bus() 之前
 * (那时 APB 时钟还开着, 寄存器还能写)。
 */
esp_err_t dsi_ext_link_reset(void);

/**
 * @brief 读通路是否已锁存故障 (曾经发生过读超时)
 *
 * 一旦锁存, dsi_ext_rx() 会立刻返回 ESP_ERR_INVALID_STATE, 不再去碰硬件 ——
 * 因为命令 FIFO 已经卡死, 再发只会继续超时。
 */
bool dsi_ext_rx_faulted(void);

/** @brief 清除读故障锁存 (只在整机重新初始化之后才有意义) */
void dsi_ext_rx_clear_fault(void);

/* ---------------- 已被 IDF 写死的硬件行为 ---------------- */

/**
 * @brief 强制 clock lane 全程 HS, 或在消隐期回 LP
 *
 * IDF 在 dpi_panel_init() 里固定设成 AUTO(消隐期回 LP)。某些面板在每次
 * 帧周期重新锁定 clock lane 时会锁到错误相位, 需要改成全程 HS。
 *
 * @param force_hs true = 全程 HS, false = 回到 IDF 默认的 AUTO
 */
esp_err_t dsi_ext_set_clock_lane(bool force_hs);

/**
 * @brief 关掉 video mode 下全部 LP 回退 (行消隐 / 场消隐 / 命令通道)
 *
 * IDF 在 dpi_panel_init() 里把这几段全开成"回 LP":
 *
 *   - lp_hbp_en / lp_hfp_en        行消隐回 LP
 *   - lp_vsa/vbp/vfp/vact_en       场消隐回 LP
 *   - lp_cmd_en                    命令通道走 LP
 *
 * 部分面板(尤其原厂例程针对 SSD2828 之类桥片调过的)要求 HS clock 全程连续,
 * 每次回 LP 都会让面板重新锁定, 表现为画面抖动/斜线/闪烁。此时需要把三段全关。
 *
 * @note 必须在 `esp_lcd_panel_init()` **之后**调用 —— 它内部会重设这几段。
 * @note 与 `dsi_ext_set_clock_lane(true)` **不等价**: 后者只改 clock lane 的
 *       `auto_clklane_ctrl` 一位, 不动 data lane 的消隐行为。两者常一起用。
 */
esp_err_t dsi_ext_disable_lp_all(void);

/** @brief 恢复 IDF 默认: 行消隐 / 场消隐 / 命令通道都允许回 LP */
esp_err_t dsi_ext_enable_lp_all(void);

/**
 * @brief 关掉帧末的 BTA (bus turnaround) ack
 *
 * IDF 默认 `frame_bta_ack_en = 1`, 每帧结束强制一次低功耗总线转向, HS clock
 * 在帧边界会中断。要求全程连续时钟的面板需要关掉它。
 *
 * @note 关掉之后 `esp_lcd_dpi_panel` 的 `on_refresh_done` 回调不会再触发
 *       (它依赖这个 ack)。
 */
esp_err_t dsi_ext_disable_frame_ack(void);

/**
 * @brief 设置 DPI 同步信号极性
 *
 * IDF 在 dpi_panel_init() 里把 5 个极性全写成 false(active-high), 没有对外
 * 开关。极性反了轻则画面偏移、重则内容错乱。
 *
 * @param hsync_low  hsync  是否低有效
 * @param vsync_low  vsync  是否低有效
 * @param de_low     DE     是否低有效
 * @param shutdn_low shut_down 是否低有效
 * @param colorm_low color_mode 是否低有效
 */
esp_err_t dsi_ext_set_timing_polarity(bool hsync_low, bool vsync_low, bool de_low,
				      bool shutdn_low, bool colorm_low);

/**
 * @brief 直接改 bridge 的像素排布字段
 *
 * bridge 的 pixel_type 寄存器有两个字段, IDF 只填了 raw_type, 另一个
 * dpi_config("pixel arrange type of dpi interface") 永远写 0。
 *
 * @param raw_type   0=rgb888 1=rgb666 2=rgb565
 * @param dpi_config 像素排布 0~3
 */
esp_err_t dsi_ext_set_pixel_type(uint8_t raw_type, uint8_t dpi_config);

/* ---------------- framebuffer 直写 ---------------- */

/**
 * @brief 取 DPI 面板内部 framebuffer 的地址
 *
 * esp_lcd_panel_draw_bitmap() 会先把用户缓冲拷进面板内部 fb 再让 DSI 扫描,
 * 那条拷贝路径(旧 IDF 是 DMA2D, 5.5 是 CPU memcpy)在排查"到底是拷贝错还是
 * 时序错"时是个干扰项。拿到内部 fb 直接写, 可以完全绕开它。
 *
 * 注意: 面板必须已经由 esp_lcd_new_panel_dpi() 创建。
 *
 * @param panel esp_lcd_panel_handle_t
 * @param fb    输出: 内部 framebuffer 首地址
 */
esp_err_t dsi_ext_fb_get(esp_lcd_panel_handle_t panel, uint8_t **fb);

/**
 * @brief 把直接写入的 framebuffer 刷到 PSRAM (cache 写回)
 *
 * PSRAM 带 cache, 直接写内部 fb 之后必须写回, 否则 DSI 扫描到的还是旧数据。
 * size 按 "宽 x 高 x 字节每像素" 自己算, 本组件不知道面板几何。
 *
 * @param fb   要写回的起始地址
 * @param size 字节数
 */
esp_err_t dsi_ext_fb_sync(const void *fb, size_t size);

/* ---------------- VPG 内置测试图案 ---------------- */

/** DSI host 内部可以自己生成的测试图案 */
typedef enum {
	DSI_EXT_PATTERN_OFF = 0,          /*!< 关闭, 恢复 framebuffer 出图 */
	DSI_EXT_PATTERN_BAR_VERTICAL,     /*!< 垂直彩条 */
	DSI_EXT_PATTERN_BAR_HORIZONTAL,   /*!< 水平彩条 */
	DSI_EXT_PATTERN_BER_VERTICAL,     /*!< 垂直误码率图案 */
} dsi_ext_pattern_t;

/**
 * @brief 让 DSI host 直接输出内置测试图案 (VPG)
 *
 * 图案由 host 内部生成, **不经过 framebuffer、不经过 bridge 的 DPI 输入通路**,
 * 所以它是划分故障域最快的手段:
 *   - 图案颜色正常 -> 链路 / PHY / 面板 / 初始化 / 时序 / color coding 都对,
 *                     问题在"我们送数据"那一段
 *   - 图案也错     -> 问题在面板侧或 color coding 配置
 *
 * 用完必须切回 DSI_EXT_PATTERN_OFF, 否则 draw_bitmap 的内容不会上屏。
 */
esp_err_t dsi_ext_vpg(esp_lcd_panel_handle_t panel, dsi_ext_pattern_t pattern);

/* ---------------- 诊断 ---------------- */

/**
 * @brief 累计的 FIFO 等待超时次数
 *
 * 每次 dsi_ext_tx() 因为 FIFO 一直满而放弃, 计数 +1。用于判断"命令发不出去"
 * 是不是面板侧不接收。
 */
uint32_t dsi_ext_timeout_count(void);

#ifdef __cplusplus
}
#endif
