/* 由 gen_init.py 从 lua 自动生成, 勿手改 */
#pragma once

#include <stdint.h>

#define PANEL_CMD_DATA_MAX 46

/* dt       : MIPI DSI 数据类型, 决定用短包还是长包、DCS 还是 Generic
 * delay_ms : 本条发完之后的延时
 * len      : data[] 的有效字节数, data[0] = 寄存器/命令
 * data     : 寄存器/命令 + 参数, 定长数组便于表格式排版 */
typedef struct {
    uint8_t dt;
    uint8_t delay_ms;
    uint8_t len;
    uint8_t data[PANEL_CMD_DATA_MAX];
} panel_cmd_t;

extern const panel_cmd_t g_panel_init[];
extern const int g_panel_init_count;
