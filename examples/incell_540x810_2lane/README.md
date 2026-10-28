# incell 540x810 · 2 lane · RGB888 —— 完整参考

一块 incell TDDI 屏（触摸与显示同一颗 die）的完整实现：
显示 + 触摸 + LVGL 自带 music demo。**这个例程可以直接当作工程模板。**

如果你只是想把一块没见过的屏点亮，先看 `bringup_minimal` —— 它更小，
不接 LVGL，排查时干扰更少。

## 跑起来

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

上电：VPG 彩条 1 秒 → music demo 播放器界面，触摸可点。

## 结构

```
main/
├── board_config.h    引脚 / 电源 / 背光 / 触摸引脚   ← 换板子只改这里
├── panel_timing.h    分辨率 / porch / lane / DPI / 格式  ← 唯一事实源
├── panel_init.c/h    初始化命令表（生成物，勿手改）
├── screen.c/h        显示：初始化、背光、纯色自检、VPG、面板句柄
├── touch.c/h         触摸：初始化、touch_read()、统计
├── lv_port.c/h       LVGL 端口：display / indev / tick / 任务
└── app_main.c        只做组装
```

依赖方向是单向的：

```
app_main ──> screen ──> dsi_ext / esp_lcd
         ──> touch  ──> i2c_master
         ──> lv_port ──> lvgl + screen_panel() + touch_read()
```

`screen.h` 和 `touch.h` **都不 include lvgl.h**，彼此也不互相 include。
只有 `lv_port` 同时认识两边 —— 它是胶水层，这是设计，不是耦合。

## app_main 的顺序（每一步都有理由）

```c
screen_power_on();     // 电源 → MIPI 开关 → DPHY LDO → 背光关
touch_early_pins();    // 触摸 SCL/SDA/INT 进高阻，让模组上拉决定电平
screen_init();         // 复位 → DSI 总线 → 初始化表 → video mode
                       //   （初始化表之后会读一次 0x0A 验证）
vTaskDelay(100);       // ★ 原厂要求：TP 寄存器读写要在 11h/29h 之后 >100ms
touch_init();          // 触摸复位 → I2C → INT 任务
lv_port_init(...);     // LVGL display/indev/tick
lv_demo_music();
lv_port_start();
screen_backlight(true); // 最后点灯，出图失败时不会先闪白
```

**触摸必须晚于显示初始化。** incell 屏的触摸和显示同一颗 die，
显示没初始化完就读写触摸寄存器，行为不确定。

## 几个关键参数（都是实测出来的）

| 参数 | 值 | 为什么 |
|---|---|---|
| `PANEL_VBP` | **6** | ⚠️ 差 2 行就整屏偏色，见 `panel_timing.h` 的长注释 |
| `PANEL_DPI_CLK_MHZ` | 40 | P4 的 DPI 只能从 240MHz 整数分频，只有 240 的约数准确 |
| `PANEL_LANE_MBPS` | 800 | 2 lane，ratio = 2.5 |
| `BOARD_PHY_LDO_MV` | 1800 | 这块板实测稳定；出花屏先回 2500 对照 |
| 像素格式 | RGB888 | framebuffer 内存排列是 **B,G,R**，不是 R,G,B |
| `BOARD_TP_I2C_HZ` | 1 MHz | 这块板实测可用；换弱上拉的板先降 400kHz |

## 踩过的坑（都写在代码注释里）

1. **VBP 影响颜色**。不是"留余量"，是面板接收状态的一部分。
2. **VPG 不能判颜色**。VPG 也要过 host 的打包器，porch 错时它一样错。
   判颜色用 `screen_fill()` 出纯色。
3. **R/B 不要交换**。`lv_color_t` 就是 `{blue, green, red}`，与 TRM 43.4.1
   的 B0 G0 R0 一致；在 flush 里交换会造成整屏反色。
4. **读 0x0A 只能在初始化表之后**。读请求发出去后面板必须回 BTA，
   不回就把命令 FIFO 永久占死，之后所有写命令超时、黑屏、软件清不掉。
5. **`CONFIG_LV_DEMO_MUSIC_AUTO_PLAY` 要关**。开着约 60 秒后 demo 会
   `lv_screen_load(lv_obj_create(NULL))` 切到空白屏，看起来像"卡住了"。
6. **分区表**。带 LVGL 的镜像 1.2MB，默认 1MB factory 装不下，
   所以本目录带了 `partitions.csv`。

## 改成你自己的屏

1. `board_config.h`：引脚、LDO 电压、触摸 I2C 地址/速率
2. `panel_timing.h`：分辨率、六个 porch（**问供应商要，不要猜**）、lane、DPI、格式
3. `panel_init.c`：换成你的初始化序列（格式说明见 `bringup_minimal/README.md`）
4. 触摸：`touch.c` 里的协议是 AXS15260 系列。换触摸 IC 就替换 `parse()` 和
   地址/寄存器定义，`touch.h` 的接口不用动

## 没有的东西

- 没有多语言、没有字体子集化 —— music demo 只用 montserrat 12/16
- 没有低功耗 / 休眠唤醒
- 没有 ESD 恢复（原厂驱动有这个模块，本工程未接）
