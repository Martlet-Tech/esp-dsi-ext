# bringup_minimal —— 点亮一块新屏的最小骨架

不带 LVGL，不假设你的面板型号。用来验证 **DSI 通路 + 面板能不能出图**，
以及**颜色对不对**。把它当成模板：三个文件改完就是你的屏。

## 跑起来

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

上电后：VPG 彩条 1 秒 → 背光开 → 纯色轮播（红/绿/蓝/白/黑，每 2 秒）。

## 要改的三个文件

| 文件 | 改什么 |
|---|---|
| `main/board_config.h` | 电源脚、复位脚、背光脚、DPHY LDO 电压。没有的写 `-1` |
| `main/panel_timing.h` | 分辨率、六个 porch、lane 数与速率、DPI 时钟、像素格式 |
| `main/panel_init.c` | 初始化命令表，把供应商给的序列逐条填进来 |

三个文件互不重叠：引脚只在 `board_config.h`，时序只在 `panel_timing.h`，
命令只在 `panel_init.c`。**别在 `screen.c` 里写死任何数字**。

### 命令表格式

```c
{ dt, delay_ms, len, { 命令, 参数... } }
```

| 资料里的写法 | 填成 |
|---|---|
| `write_reg(0xBB, {0x01,0x02})` | `{0x39, 0, 3, {0xBB, 0x01, 0x02}}` |
| `{0x11}` 无参数 | `{0x05, 120, 1, {0x11}}` |
| `{0x51, 0xFF}` 一个参数 | `{0x15, 0, 2, {0x51, 0xFF}}` |
| 面板不认 DCS | 把 `0x05/0x15/0x39` 换成 `0x03/0x13/0x29` |

表尾必须有 `11h`（sleep out，等 120ms）和 `29h`（display on，等 20ms）。

## 判读

串口每 2 秒打一行"这一帧应该是什么颜色"，对照屏幕：

| 屏幕显示 | 结论 | 下一步 |
|---|---|---|
| 红就是红 | 全对 | — |
| 红显示成**蓝** | R/B 通道反了 | 检查 framebuffer 是否按 B,G,R 写（`screen_fill` 已按 BGR） |
| 红显示成**青** | 整屏反相 | 查 `3Ah` / 面板 invert 命令 |
| 红显示成**粉/暗红** | 位打包或位宽错 | 查 `panel_timing.h` 的 `PANEL_BITS_PER_PIXEL` 与 porch |
| 全黑 | 没出图 | 看串口：初始化表第几条失败、`[10] 读 0x0A` 有没有值 |
| 画面斜线/错位 | 时序或时钟截断 | 见 `esp32p4-dsi-debug` skill 的第四节 |

## VPG 的边界（容易踩）

VPG 彩条由 DSI host 内部生成，不经过 framebuffer，所以能快速区分
"面板/链路"和"我们送的数据"。但它**也要过 host 的打包器**——
porch 错的时候 VPG 一样错。所以：

- **VPG 正常** → 几何、出图、面板初始化没问题；**不能**说明颜色对
- **VPG 也错** → 问题在 porch / 时钟 / 面板侧，不在你的 framebuffer

判颜色一定用 `screen_fill()` 出纯色。

## 关于读 0x0A

初始化表发完后会读一次 `0x0A`（RDDPM），这是"初始化到底生效没有"最硬的证据：

```
[10] 读 0x0A = 0x9C (1 字节) | booster=1 sleep_out=1 normal=1 display_on=1
```

读到 `0x00` / `0xFF` / 超时 → 读通路没通（面板不支持读，或者读请求速度不对）。
`screen.c` 里已经调了 `dsi_ext_set_read_speed(true)`，让读请求跟着写命令走 LP；
如果还是读不到，面板可能确实不支持读，把 `SCR_READ_RDDPM` 置 0 即可。

> ⚠️ 读请求发出去以后面板必须回 BTA，否则 host 的命令 FIFO 会被永久占死，
> **之后所有写命令都超时、屏幕黑，且软件清不掉**。所以读只能放在初始化表
> 全部发完之后，绝不能在表中间读。
