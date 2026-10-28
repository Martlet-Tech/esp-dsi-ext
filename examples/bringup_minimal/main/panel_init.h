/* 面板初始化表 —— 数据定义（与具体面板无关的部分） */
#pragma once

#include <stdint.h>

/* 单条命令最长参数个数。屏厂给的表里最长的一条通常 30~40 字节，
 * 46 是实测够用的值，改大只是多占 flash。 */
#define PANEL_CMD_DATA_MAX  46

/**
 * 一条初始化命令。
 *
 * dt       MIPI DSI 数据类型，决定包格式：
 *            0x05 DCS short write, 0 参数
 *            0x15 DCS short write, 1 参数
 *            0x39 DCS long write
 *            0x03 / 0x13 / 0x23 Generic short write (0/1/2 参数)
 *            0x29 Generic long write
 *          屏厂资料按 DCS 写的就用 0x05/0x15/0x39；面板不认 DCS 时
 *          整表换成 0x03/0x13/0x29（把 dt 换掉即可，data 不用动）。
 * delay_ms 本条发完之后的延时
 * len      data[] 有效字节数，data[0] = 命令，其余是参数
 */
typedef struct {
    uint8_t dt;
    uint8_t delay_ms;
    uint8_t len;
    uint8_t data[PANEL_CMD_DATA_MAX];
} panel_cmd_t;

extern const panel_cmd_t g_panel_init[];
extern const int g_panel_init_count;
