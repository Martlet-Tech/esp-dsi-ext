/*
 * dsi_ext 实现 —— 直接吃 hal 的私有 inline 接口, 不动 IDF 源码
 *
 * 依赖关系: PRIV_REQUIRES hal soc
 *   hal -> hal/mipi_dsi_host_ll.h, hal/mipi_dsi_brg_ll.h
 *   soc -> dsi_host_dev_t / dsi_brg_dev_t 寄存器结构体, MIPI_DSI_BRIDGE 实例
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "dsi_ext.h"

static const char *TAG = "dsi_ext";

#if SOC_MIPI_DSI_SUPPORTED

#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_rom_sys.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_phy_ll.h"
#include "hal/mipi_dsi_brg_ll.h"

static dsi_host_dev_t *s_host;
static dsi_brg_dev_t  *s_brg;
static uint32_t        s_timeout_cnt;
static bool            s_rx_faulted;      /* 读超时后锁存: FIFO 已卡死, 别再试 */
static bool            s_bta_to_set;      /* 是否已给 host 配过 BTA 超时 */

/* 默认 BTA 超时: 0x4000 个 lane byte clock 周期。
 * 800Mbps 下 byteclk = 100MHz, 即约 164us —— 远大于一次正常 BTA (us 级),
 * 又远小于我们的软件超时 (100ms), 足够让硬件先放弃。 */
#define DSI_EXT_DEFAULT_BTA_TO 0x4000

/* 带超时的 FIFO 等待。
 * 原版 IDF 里这些地方都是裸 while, 面板不回应时会死等并触发 task_wdt,
 * 这里一律加上限, 卡住就报错返回。 */
static bool wait_cmd_fifo(dsi_host_dev_t *host)
{
	for (int i = 0; i < 500000; i++) {
		if (!mipi_dsi_host_ll_gen_is_cmd_fifo_full(host)) {
			return true;
		}
	}
	s_timeout_cnt++;
	ESP_LOGE(TAG, "命令 FIFO 一直满, 放弃");
	return false;
}

static bool wait_pld_fifo(dsi_host_dev_t *host)
{
	for (int i = 0; i < 500000; i++) {
		if (!mipi_dsi_host_ll_gen_is_write_fifo_full(host)) {
			return true;
		}
	}
	s_timeout_cnt++;
	ESP_LOGE(TAG, "写 FIFO 一直满, 放弃");
	return false;
}

esp_err_t dsi_ext_init(int port)
{
	dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(port);
	if (host == NULL) {
		ESP_LOGE(TAG, "取不到 DSI host 指针 (port=%d)", port);
		return ESP_ERR_NOT_FOUND;
	}
	s_host = host;
	s_brg = &MIPI_DSI_BRIDGE;
	s_timeout_cnt = 0;
	/* 总线是新建的 (或刚被 link_reset), 寄存器都回默认值, 状态清零 */
	s_rx_faulted = false;
	s_bta_to_set = false;
	return ESP_OK;
}

void *dsi_ext_host(void)
{
	return s_host;
}

static void set_all_wr_speed(dsi_host_dev_t *host, mipi_dsi_ll_trans_speed_mode_t speed)
{
	mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(host, 0, speed);
	mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(host, 1, speed);
	mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(host, speed);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(host, 0, speed);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(host, 1, speed);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(host, 2, speed);
	mipi_dsi_host_ll_set_gen_long_wr_speed_mode(host, speed);
}

esp_err_t dsi_ext_cmd_channel_config(bool cmd_ack, bool te_ack,
				     dsi_ext_wr_speed_t wr_speed)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	mipi_dsi_host_ll_enable_cmd_ack(s_host, cmd_ack);
	mipi_dsi_host_ll_enable_te_ack(s_host, te_ack);

	switch (wr_speed) {
	case DSI_EXT_WR_SPEED_KEEP:
		/* 一个都不改 —— 保留调用前(通常是 IDF 设的 LP)的值 */
		break;
	case DSI_EXT_WR_SPEED_LP:
		set_all_wr_speed(s_host, MIPI_DSI_LL_TRANS_SPEED_LP);
		break;
	case DSI_EXT_WR_SPEED_HS:
		set_all_wr_speed(s_host, MIPI_DSI_LL_TRANS_SPEED_HS);
		break;
	default:
		ESP_LOGE(TAG, "未知的写速度 %d", (int)wr_speed);
		return ESP_ERR_INVALID_ARG;
	}

	return ESP_OK;
}

esp_err_t dsi_ext_disable_cmd_ack(void)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	/* 只关 ACK; 写速度保持 IDF 设好的 LP, 一个位都不碰 */
	mipi_dsi_host_ll_enable_cmd_ack(s_host, false);
	return ESP_OK;
}

esp_err_t dsi_ext_tx(uint8_t vc, uint8_t dt, const uint8_t *data, uint16_t len)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (data == NULL || len == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	uint8_t msb = 0, lsb = 0;
	bool is_long = (dt == 0x39) || (dt == 0x29);

	if (is_long && len > 2) {
		/* 长包: payload 按 4 字节对齐压入写 FIFO (DWC 要求按字写) */
		for (uint16_t i = 0; i < len; i += 4) {
			uint32_t w = 0;
			for (int k = 0; k < 4; k++) {
				if (i + k < len) {
					w |= (uint32_t)data[i + k] << (8 * k);
				}
			}
			if (!wait_pld_fifo(s_host)) {
				return ESP_ERR_TIMEOUT;
			}
			mipi_dsi_host_ll_gen_write_payload_fifo(s_host, w);
		}
		msb = (len >> 8) & 0xFF;
		lsb = len & 0xFF;
	} else {
		/* 短包 (含"指定长包但长度不足 3"的退化情况) */
		uint16_t h = 0;
		if (len >= 1) {
			h = data[0];
		}
		if (len >= 2) {
			h |= (uint16_t)data[1] << 8;
		}
		msb = (h >> 8) & 0xFF;
		lsb = h & 0xFF;
	}

	if (!wait_cmd_fifo(s_host)) {
		return ESP_ERR_TIMEOUT;
	}
	mipi_dsi_host_ll_gen_set_packet_header(s_host, vc, (mipi_dsi_data_type_t)dt, msb, lsb);
	return ESP_OK;
}

esp_err_t dsi_ext_set_clock_lane(bool force_hs)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	mipi_dsi_host_ll_set_clock_lane_state(s_host,
			force_hs ? MIPI_DSI_LL_CLOCK_LANE_STATE_HS
				 : MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
	return ESP_OK;
}

esp_err_t dsi_ext_disable_lp_all(void)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	/* 三段全关: 行消隐 (HBP/HFP) / 场消隐 (VSYNC/VBP/VFP/VACT) / 命令通道。
	 * 与 IDF 默认行为相反 —— IDF 在 dpi_panel_init() 里把这几段全开成回 LP。 */
	mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(s_host, false, false);
	mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(s_host, false, false, false, false);
	mipi_dsi_host_ll_dpi_enable_lp_command(s_host, false);
	return ESP_OK;
}

esp_err_t dsi_ext_enable_lp_all(void)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	/* 恢复 IDF 默认: 各段都允许回 LP。 */
	mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(s_host, true, true);
	mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(s_host, true, true, true, true);
	mipi_dsi_host_ll_dpi_enable_lp_command(s_host, true);
	return ESP_OK;
}

esp_err_t dsi_ext_disable_frame_ack(void)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	mipi_dsi_host_ll_dpi_enable_frame_ack(s_host, false);
	return ESP_OK;
}

esp_err_t dsi_ext_set_timing_polarity(bool hsync_low, bool vsync_low, bool de_low,
				      bool shutdn_low, bool colorm_low)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	mipi_dsi_host_ll_dpi_set_timing_polarity(s_host, hsync_low, vsync_low, de_low,
						 shutdn_low, colorm_low);
	return ESP_OK;
}

esp_err_t dsi_ext_set_pixel_type(uint8_t raw_type, uint8_t dpi_config)
{
	if (s_brg == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	s_brg->pixel_type.raw_type = raw_type & 0x0F;
	s_brg->pixel_type.dpi_config = dpi_config & 0x03;
	mipi_dsi_brg_ll_update_dpi_config(s_brg);
	return ESP_OK;
}

/* ---------------- 读 ---------------- */

/* 在 timeout_ms 内等一个条件成立 */
static bool wait_until(bool (*pred)(dsi_host_dev_t *), dsi_host_dev_t *host, uint32_t timeout_ms)
{
	int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
	while (!pred(host)) {
		if (esp_timer_get_time() > deadline) {
			return false;
		}
	}
	return true;
}

static bool pred_not_busy(dsi_host_dev_t *h)  { return !mipi_dsi_host_ll_gen_is_read_cmd_busy(h); }
static bool pred_fifo_has(dsi_host_dev_t *h)  { return !mipi_dsi_host_ll_gen_is_read_fifo_empty(h); }

esp_err_t dsi_ext_rx(uint8_t vc, uint8_t dt, const uint8_t *req, uint8_t req_len,
		     void *buf, uint16_t read_len, uint32_t timeout_ms,
		     uint16_t *actual_len)
{
	if (actual_len) {
		*actual_len = 0;
	}
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (s_rx_faulted) {
		ESP_LOGW(TAG, "读: 之前已发生读超时(命令 FIFO 已卡死), 本次直接返回");
		return ESP_ERR_INVALID_STATE;
	}
	if (buf == NULL || read_len == 0 || req_len > 2) {
		return ESP_ERR_INVALID_ARG;
	}
	if (req_len > 0 && req == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	/* 没有 BTA 超时就先配一个默认值 —— 否则面板不应答会把命令 FIFO 永久锁死 */
	if (!s_bta_to_set) {
		dsi_ext_enable_bta_timeout(DSI_EXT_DEFAULT_BTA_TO, DSI_EXT_DEFAULT_BTA_TO);
		ESP_LOGW(TAG, "读: 已给 host 打开 BTA 超时 (0x%X 周期), 防止读事务无限挂起",
			 DSI_EXT_DEFAULT_BTA_TO);
	}

	/* 保存 video mode 状态: 读之前必须切到命令模式, 读完要还原 */
	bool was_video = !s_host->mode_cfg.cmd_video_mode;

	mipi_dsi_host_ll_enable_video_mode(s_host, false);
	mipi_dsi_host_ll_enable_bta(s_host, true);
	mipi_dsi_host_ll_gen_set_rx_vcid(s_host, vc);

	/* 先告诉面板"最多回多少字节" (SET_MAXIMUM_RETURN_PKT = 0x37) */
	uint8_t max_ret[2] = { (uint8_t)(read_len & 0xFF), (uint8_t)(read_len >> 8) };
	esp_err_t err = dsi_ext_tx(vc, 0x37, max_ret, 2);
	if (err != ESP_OK) {
		goto restore;
	}

	/* 发读请求: 短包, header 按 req_len 自动拼 (0/1/2 字节) */
	{
		uint8_t hdr[2] = { 0, 0 };
		for (uint8_t i = 0; i < req_len; i++) {
			hdr[i] = req[i];
		}
		err = dsi_ext_tx(vc, dt, hdr, req_len ? req_len : 1);
		if (err != ESP_OK) {
			goto restore;
		}
	}

	/* 等命令发完 */
	if (!wait_until(pred_not_busy, s_host, timeout_ms)) {
		s_timeout_cnt++;
		ESP_LOGE(TAG, "读: 命令一直忙 (dt=0x%02X), %u ms 超时 —— 面板没回 BTA "
			 "且硬件超时没生效, 命令 FIFO 已卡死 (锁存故障)", dt, (unsigned)timeout_ms);
		s_rx_faulted = true;
		err = ESP_ERR_TIMEOUT;
		goto restore;
	}

	/* 等第一包数据 */
	if (!wait_until(pred_fifo_has, s_host, timeout_ms)) {
		s_timeout_cnt++;
		ESP_LOGE(TAG, "读: 数据没来 (dt=0x%02X), %u ms 超时", dt, (unsigned)timeout_ms);
		s_rx_faulted = true;
		err = ESP_ERR_TIMEOUT;
		goto restore;
	}

	/* 把 read FIFO 全部取空, 按小端拆字节, 超过 read_len 的丢弃 */
	{
		uint8_t *out = (uint8_t *)buf;
		uint16_t n = 0;
		while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(s_host)) {
			uint32_t w = mipi_dsi_host_ll_gen_read_payload_fifo(s_host);
			for (int i = 0; i < 4 && n < read_len; i++) {
				out[n++] = (uint8_t)((w >> (8 * i)) & 0xFF);
			}
		}
		if (actual_len) {
			*actual_len = n;
		}
		if (n == 0) {
			ESP_LOGW(TAG, "读: FIFO 里一个字节都没有 (dt=0x%02X)", dt);
		}
	}

restore:
	/* 恢复现场: BTA 关掉, video mode 回到调用前的状态 */
	mipi_dsi_host_ll_enable_bta(s_host, false);
	mipi_dsi_host_ll_enable_video_mode(s_host, was_video);
	return err;
}

esp_err_t dsi_ext_set_read_speed(bool lp)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	mipi_dsi_ll_trans_speed_mode_t m = lp ? MIPI_DSI_LL_TRANS_SPEED_LP
					      : MIPI_DSI_LL_TRANS_SPEED_HS;

	/* 读类数据包的速度位, IDF 全都不碰 -> 复位值 0 = HS */
	mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(s_host, 0, m);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(s_host, 0, m);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(s_host, 1, m);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(s_host, 2, m);
	/* 0x37 SET_MAXIMUM_RETURN_PKT 也有独立的传输速度位 (bit24) */
	mipi_dsi_host_ll_set_mrps_speed_mode(s_host, m);
	return ESP_OK;
}

esp_err_t dsi_ext_set_max_read_time(uint32_t byte_clocks)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (byte_clocks == 0) {
		return ESP_OK;
	}
	mipi_dsi_phy_ll_set_max_read_time(s_host, byte_clocks);
	return ESP_OK;
}

esp_err_t dsi_ext_link_reset(void)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	/* 关闭状态会把数字逻辑整体复位 (TRM 43.4.3.1.2) */
	mipi_dsi_host_ll_power_on_off(s_host, false);
	mipi_dsi_phy_ll_power_on_off(s_host, false);
	esp_rom_delay_us(1000);
	mipi_dsi_host_ll_power_on_off(s_host, true);
	mipi_dsi_phy_ll_power_on_off(s_host, true);
	mipi_dsi_phy_ll_reset(s_host);
	mipi_dsi_phy_ll_enable_clock_lane(s_host, true);
	/* 寄存器已回默认值, 本组件内部状态跟着清零, 调用方需重新走一遍 bringup */
	s_rx_faulted = false;
	s_bta_to_set = false;
	return ESP_OK;
}

esp_err_t dsi_ext_enable_bta_timeout(uint16_t bta_cycles, uint16_t lp_rx_cycles)
{
	if (s_host == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	/* hs_rd / lp_rd / bta 三者的单位都是 lane byte clock, 一起设成同一个值;
	 * 写方向的 hs_tx / hs_wr / lp_wr 保持 IDF 的 0 (禁用), 不影响正常发命令。 */
	mipi_dsi_host_ll_set_timeout_count(s_host, 0, lp_rx_cycles,
					    bta_cycles, bta_cycles, 0, 0, bta_cycles);
	s_bta_to_set = (bta_cycles != 0);
	return ESP_OK;
}

bool dsi_ext_rx_faulted(void)
{
	return s_rx_faulted;
}

void dsi_ext_rx_clear_fault(void)
{
	s_rx_faulted = false;
}

esp_err_t dsi_ext_rx_dcs(uint8_t cmd, void *buf, uint16_t read_len,
			 uint32_t timeout_ms, uint16_t *actual_len)
{
	return dsi_ext_rx(0, 0x06, &cmd, 1, buf, read_len, timeout_ms, actual_len);
}

esp_err_t dsi_ext_fb_get(esp_lcd_panel_handle_t panel, uint8_t **fb)
{
	if (panel == NULL || fb == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	void *p = NULL;
	esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &p);
	if (err != ESP_OK) {
		return err;
	}
	*fb = (uint8_t *)p;
	return ESP_OK;
}

esp_err_t dsi_ext_fb_sync(const void *fb, size_t size)
{
	if (fb == NULL || size == 0) {
		return ESP_ERR_INVALID_ARG;
	}
	return esp_cache_msync((void *)fb, size,
			       ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

esp_err_t dsi_ext_vpg(esp_lcd_panel_handle_t panel, dsi_ext_pattern_t pattern)
{
	if (panel == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	static const mipi_dsi_pattern_type_t map[] = {
		[DSI_EXT_PATTERN_OFF]            = MIPI_DSI_PATTERN_NONE,
		[DSI_EXT_PATTERN_BAR_VERTICAL]   = MIPI_DSI_PATTERN_BAR_VERTICAL,
		[DSI_EXT_PATTERN_BAR_HORIZONTAL] = MIPI_DSI_PATTERN_BAR_HORIZONTAL,
		[DSI_EXT_PATTERN_BER_VERTICAL]   = MIPI_DSI_PATTERN_BER_VERTICAL,
	};
	if ((int)pattern < 0 || pattern > DSI_EXT_PATTERN_BER_VERTICAL) {
		return ESP_ERR_INVALID_ARG;
	}
	return esp_lcd_dpi_panel_set_pattern(panel, map[pattern]);
}

uint32_t dsi_ext_timeout_count(void)
{
	return s_timeout_cnt;
}

#else /* !SOC_MIPI_DSI_SUPPORTED */

esp_err_t dsi_ext_init(int port) { (void)port; return ESP_ERR_NOT_SUPPORTED; }
void *dsi_ext_host(void) { return NULL; }
esp_err_t dsi_ext_cmd_channel_config(bool a, bool b, dsi_ext_wr_speed_t c) { (void)a; (void)b; (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_disable_cmd_ack(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_tx(uint8_t vc, uint8_t dt, const uint8_t *d, uint16_t n) { (void)vc; (void)dt; (void)d; (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_set_clock_lane(bool hs) { (void)hs; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_disable_lp_all(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_enable_lp_all(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_disable_frame_ack(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_set_timing_polarity(bool a, bool b, bool c, bool d, bool e) { (void)a; (void)b; (void)c; (void)d; (void)e; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_set_pixel_type(uint8_t r, uint8_t c) { (void)r; (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_rx(uint8_t vc, uint8_t dt, const uint8_t *r, uint8_t rl, void *b, uint16_t n, uint32_t t, uint16_t *a)
{ (void)vc; (void)dt; (void)r; (void)rl; (void)b; (void)n; (void)t; if (a) *a = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_rx_dcs(uint8_t c, void *b, uint16_t n, uint32_t t, uint16_t *a)
{ (void)c; (void)b; (void)n; (void)t; if (a) *a = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_enable_bta_timeout(uint16_t b, uint16_t l) { (void)b; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_set_read_speed(bool lp) { (void)lp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_set_max_read_time(uint32_t n) { (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_link_reset(void) { return ESP_ERR_NOT_SUPPORTED; }
bool dsi_ext_rx_faulted(void) { return false; }
void dsi_ext_rx_clear_fault(void) { }
esp_err_t dsi_ext_fb_get(esp_lcd_panel_handle_t p, uint8_t **fb) { (void)p; (void)fb; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_fb_sync(const void *fb, size_t n) { (void)fb; (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dsi_ext_vpg(esp_lcd_panel_handle_t p, dsi_ext_pattern_t t) { (void)p; (void)t; return ESP_ERR_NOT_SUPPORTED; }
uint32_t dsi_ext_timeout_count(void) { return 0; }

#endif /* SOC_MIPI_DSI_SUPPORTED */
