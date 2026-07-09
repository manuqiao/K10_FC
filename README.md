# UNIHIKER K10 — FC/NES 模拟器（nofrendo，移植自 retro-go）

在 DFRobot 行空板 UNIHIKER K10 上运行 `games/` 目录下打包的 NES/FC 游戏，核心使用
[nofrendo](https://github.com/ducalex/retro-go/tree/master/retro-core/components/nofrendo)
（来自 [ducalex/retro-go](https://github.com/ducalex/retro-go)）。开机后先选控制方式，再
进入游戏列表——选好后即可开玩。

K10 是一块 ESP32-S3 主板（16 MB 闪存 / 8 MB PSRAM），带 2.8" **ILI9341** 320×240 横屏、
I2S 功放喇叭、两个按键（A/B）以及 SC7A20H 加速度计和光敏传感器。所以 retro-go 在
ODROID-GO 上用的「ILI9341 + I2S + ESP32」方案几乎可以原样搬过来。

工程基于 PlatformIO / Arduino 构建，零警告通过编译；烧录后即玩。

> 关联项目（同一作者，一对手柄端 / 主机端工程）：
> - **[BleJoystick](https://gitcode.com/Xelocity/BleJoystick)** ——**配套的安卓手柄 App**，把安卓
>   手机变成一个 BLE 手柄外设（屏幕虚拟 FC 手柄，或把插在手机上的 USB Xbox 手柄映射出去），经
>   BLE GATT Notify 推送按键事件。它就是「自定义蓝牙手柄」模式**开箱即用的手柄端**——发出的正是
>   下文 [FFF0/FFF1 字节协议](#蓝牙手柄与按键映射表)，与本工程对接无需自己写固件。
> - **[K10_Joystick](https://gitcode.com/Xelocity/K10_Joystick)** ——K10 蓝牙手柄**监听**工程。
>   `libs/BLE_FFF0/` 即从该项目中抽取、剥离界面代码后整理成的可复用 BLE Central 库。

---

## 开机流程

```
上电 → 控制方式选择 → （蓝牙模式：扫描/选择/连接手柄）→ 游戏列表 → 运行游戏
```

1. **控制方式选择**（始终由板上按键操作）——四选一循环：
   - 板上 **A**：在「本机控制」/「蓝牙手柄(自定义)」/「蓝牙 HID 手柄」/「矩阵键盘」之间切换
   - 板上 **B**：确认
2. **蓝牙手柄模式**（自定义 / HID 两种都走同一套流程）：进入扫描界面（8 秒），
   列出按信号强度排序的设备，板上按键操作：
   - 板上 **A**：选择下一个设备
   - 板上 **B**：连接
   - **长按 A+B**：重新扫描
3. **游戏列表**：列出 `games/` 中所有 `.nes`（按文件名排序）

   | 模式 | 移动高亮 | 启动游戏 |
   |------|----------|----------|
   | 本机控制（板上按键） | 板上 **A** | 板上 **B** |
   | 蓝牙手柄（自定义 / HID） | 手柄 **上/下** | 手柄 **A** 或 **Start** |

---

## 操控方式

### 本机控制（板载传感器 + 两颗按键）

K10 没有方向键，所以方向靠倾斜主板、其它按键靠两颗物理键和光敏传感器凑出来：

| 游戏动作 | NES 按键 | 操作方式 |
|----------|----------|----------|
| 移动 | 方向键 | **倾斜主板**（左/右/上/下） |
| 跳跃 | A | 板上按键 **B** |
| 开火 | B | 板上按键 **A** |
| 开始 / 暂停 | START | **遮挡光敏传感器**（环境光低于阈值时持续触发） |
| 选择 | SELECT | **同时按住 A + B** 约 0.6 秒 |

> 注：板上 **B → NES A**（跳跃），板上 **A → NES B**（开火），与按键丝印相反，是按游戏手感
> 专门映射的。倾斜的灵敏度、轴向、正负号均在 `src/k10_input.cpp` 顶部可调。

### 蓝牙手柄

连接手柄后，操控权交给手柄：方向键移动、A/B 跳跃开火、Start/Select 按标注使用。手柄掉线时
输入归零（不会卡键），库会自动尝试重连（最多 3 次）。

> 没有实体手柄也行：在安卓手机上装配套的 **[BleJoystick](https://gitcode.com/Xelocity/BleJoystick)**
> App，手机本身就变成这只「自定义蓝牙手柄」（屏幕虚拟 FC 手柄，或把插在手机上的 USB Xbox 手柄
> 映射出去），K10 直接扫到、连上即用。详见 [按键映射表](#蓝牙手柄与按键映射表)。

### 蓝牙 HID 手柄（标准 HID 协议）

与上面的「自定义蓝牙手柄」（HM-10 透串 FFF0/FFF1 字节协议）并列的第二种蓝牙模式，面向**标准
BLE HID 手柄**（HID 服务 `0x1812`）。开机选「Bluetooth HID gamepad」后走同样的扫描/选择/连接
流程：K10 作为 BLE 主机与手柄**配对绑定**（just-works，密钥写入 NVS，重启后重连免重新配对），
订阅手柄的 Report 特征值，并解析 HID Report Map 自动识别按键位与方向键（hat）布局。

- 方向键取自 HID hat（Usage `0x39`）；A/B/Start/Select 取自按键字段（Usage Page `0x09`），
  经 `src/k10_ble_hid.cpp` 顶部的 `BUTTON2NES[]` 表按 Android 约定映射（Btn1=A、Btn2=B、
  Btn7=Select、Btn8=Start）。
- **按键不对就调表**：廉价手柄的按键编号并不统一。把 `libs/BLE_HID_Host/BLE_HID_Host.cpp` 顶部
  的 `K10_BLE_HID_DEBUG` 置 1，串口会打印每帧原始字节与解析结果（和当初抓 FFF0 协议同一套办法），
  照着改 `BUTTON2NES[]` 即可。
- K10 是 ESP32-S3，**只支持 BLE**：能连标准 BLE HID 手柄；经典蓝牙手柄（Xbox / PS 原装走经典
  蓝牙的那类）仍连不上。

> 完整的连接/配对/订阅/解码流程（含踩过的两个坑、Report Map 解析、报文特判、API 表）见
> [`libs/BLE_HID_Host/README.md`](libs/BLE_HID_Host/README.md)。

---

## 蓝牙手柄与按键映射表

蓝牙手柄支持由 `libs/BLE_FFF0/` 提供，它是一个面向 HM-10 类透传串口外设的极简 BLE Central，
对接 **Service `0xFFF0` / Characteristic `0xFFF1`** 的通知（notify）数据。手柄每按下一颗键，
K10 就会从 `0xFFF1` 收到一个或多个字节的事件包，每个字节编码一颗按键的一次状态变化：

```
字节 = (按下 ? 0x80 : 0x00) | code
        └─ bit7：按键状态（1 = 按下，0 = 松开）
           └─ bit6..0：按键代码 code（取值 1..8）
```

`code` 为 0 或大于 8 的字节会被忽略（不是按键事件）。一个数据包内可能包含多次按键事件，
代码会逐字节解析、合并到当前 NES 手柄状态字节里。完整映射见 `src/k10_ble.cpp` 中的
`CODE2NES[]`：

| code | 手柄按键 | NES 位 | NES 位掩码 | 按下时字节 | 松开时字节 |
|------|----------|--------|-----------|-----------|-----------|
| `0x01` | ↑ Up | NP_UP | `0x10` | `0x81` | `0x01` |
| `0x02` | ↓ Down | NP_DOWN | `0x20` | `0x82` | `0x02` |
| `0x03` | ← Left | NP_LEFT | `0x40` | `0x83` | `0x03` |
| `0x04` | → Right | NP_RIGHT | `0x80` | `0x84` | `0x04` |
| `0x05` | A | NP_A | `0x01` | `0x85` | `0x05` |
| `0x06` | B | NP_B | `0x02` | `0x86` | `0x06` |
| `0x07` | Select | NP_SELECT | `0x04` | `0x87` | `0x07` |
| `0x08` | Start | NP_START | `0x08` | `0x88` | `0x08` |

也就是说：想让自制的蓝牙手柄被本工程识别，只要以 HM-10 透传方式连接、并在 `0xFFF1` 上按上表
发出对应字节即可。**不想自己写手柄固件的话**，直接用配套的安卓 App
**[BleJoystick](https://gitcode.com/Xelocity/BleJoystick)**——它就是把安卓手机变成这只手柄，
按上表约定经 GATT Notify 推送字节，无需自己实现协议。多键同时按下时，按状态变化的先后逐字节
发送（按下发 `0x80 | code`，松开发 `code`）。`BLE_FFF0` 库对 `0xFFF1` 还做了降级兼容
（128 位 UUID 含 `fff1`、`0xFFF0` 下第一个可通知特征、乃至全设备探测模式），详见
`libs/BLE_FFF0/README.md`。

---

## 构建 / 烧录 / 串口监视

使用本机已安装的 K10 PlatformIO（`~/K10P/pio`）：

```bash
~/K10P/pio run                                                    # 构建
~/K10P/pio device list                                            # 找到 K10 串口
~/K10P/pio run -t upload --upload-port /dev/cu.usbmodemXXXX       # 烧录
~/K10P/pio device monitor                                         # 115200：开机日志 + 每秒性能
```

`games/` 下的每一个 `.nes` 会在**编译期**被嵌入固件（`tools/gen_rom_catalog.py` 生成
`src/rom_catalog{,_data}.h`），一次烧录即包含全部游戏——无需 SD 卡、无需额外的文件系统步骤。

> 若某次 Arduino 烧录后 K10 变砖，用 Mind+ 的「恢复设备初始设置」救回，再重新烧录即可。

---

## 首次调试（仅在画面/声音/手感不对时）

均为一行改动，且改动处都有注释说明：

- **画面上下颠倒** → `src/k10_video.cpp`：`tft.setRotation(1)` 改为 `(3)`（或反过来）。
- **红蓝颜色对调** → `src/k10_video.cpp`：`tft.pushColors(..., true)` 改为 `false`。
- **倾斜方向错 / 自行走位** → `src/k10_input.cpp`：调整 `TILT_DZ`，或翻转 `LR_SIGN` / `UD_SIGN`，
  或交换 `AXIS_LR` / `AXIS_UD`。
- **光敏触发 Start 不可靠** → `src/k10_input.cpp`：调 `ALS_START_THRESHOLD`（先看串口 `[als]`
  打印的环境光读数），或改用「A+B → START」的映射。
- **Select 误触** → `src/k10_input.cpp`：调大 `BOTH_HOLD_MS`（设 0 为立即触发）。

---

## 性能说明

SPI 屏默认跑 40 MHz（框架 TFT_eSPI 默认值），帧率可玩但不够丝滑。若要更高帧率，可把
ILI9341 的 SPI 时钟提到 80 MHz：

```
# K10 框架（本机共享路径）：
# /Users/mac/K10P/.platformio/packages/framework-arduinounihiker/libraries/TFT_eSPI/User_Setup.h
#define SPI_FREQUENCY  80000000   # 原为 40000000
```

若 80 MHz 下出现花屏/撕裂，就退回 40 MHz。视频帧率还受 `src/main.cpp` 顶部的 `BLIT_EVERY_N`
控制（默认每 3 帧渲染一次画面，但每帧都出声，以保证音频 60 Hz 平滑）；想再提视频帧率，应优先
把约 21 ms 的 SPI blit 改成异步 DMA，而不是调小 `BLIT_EVERY_N`（否则会破音）。

---

## 增删游戏

把 `.nes` 文件丢进 `games/` 重新编译即可——构建会自动发现目录下全部 `*.nes`、全部嵌入，
开机菜单按文件名顺序列出：

```bash
cp "MyGame.nes" games/
~/K10P/pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

不想完整编译时，可单独跑生成器（`python3 tools/gen_rom_catalog.py`）。大 ROM 没问题——app
分区还有约 4 MB 余量（自带两个示例 ROM 的完整固件只占用约 15% 闪存）。

---

## 目录结构

```
platformio.ini            K10 环境（Arduino、USB CDC、Model=None、PSRAM 经 board 开启）
games/*.nes               ROM 库——编译期全部嵌入固件
libs/BLE_FFF0/            可复用 BLE Central 库（自定义 HM-10 透串协议，抽取自 K10_Joystick）
libs/BLE_HID_Host/        可复用 BLE HID 主机库（标准 HID 手柄 0x1812：配对/订阅/Report Map 解析，
                          连接流程详见其 README.md）
tools/gen_rom_catalog.py  编译期生成器（extra_scripts）：扫描 games/*.nes，写出
                          src/rom_catalog{,_data}.h
src/
  main.cpp                开机、控制方式选择、ROM 加载、NES 帧循环
  k10_menu.{h,cpp}        控制方式选择屏 + 游戏选择屏
  k10_ble.{h,cpp}         蓝牙手柄输入源 + 扫描/配对 UI（自定义 FFF0 协议，基于 BLE_FFF0）
  k10_ble_hid.{h,cpp}     蓝牙 HID 手柄输入源 + 扫描/配对 UI（标准 HID，基于 BLE_HID_Host）
  rom_catalog.h           生成：RomEntry + extern 表
  rom_catalog_data.h      生成：ROM 的 PROGMEM 数组 + 表定义
  k10_video.{h,cpp}       NES 调色板 → RGB565 → ILI9341（TFT_eSPI，横屏）
  k10_audio.{h,cpp}       I2S 喇叭输出（BCLK0/WS38/DOUT45/MCLK3）
  k10_input.{h,cpp}       倾斜（加速度计）+ A/B + 光敏 → NES 手柄
lib/nofrendo/             retro-go 的 nofrendo NES 核心；仅 nes/utils.h 被替换
                          （独立 shim，不依赖 retro-go）
```

## 移植原理

- **核心**：retro-go 的 nofrendo 是纯 C；原版只有 `nes/utils.h` 依赖 retro-go（用 `rg_system.h`
  做日志/CRC32）。这一文件被替换成独立 shim，于是整个核心在 Arduino 下能不改动地编译。
- **视频**：K10 的屏是 TFT_eSPI 驱动的 ILI9341，正是 retro-go 的目标控制器。`k10video::blit`
  作为 nofrendo 的逐帧回调，把 256 色索引帧缓冲经 RGB565 调色板转换后推送到 LCD。
- **音频**：`k10audio` 把 I2S_NUM_0（K10 引脚）重配为喇叭输出，流式播放 nofrendo 每帧的 APU 采样。
- **输入**：`k10input`（本机）/ `k10ble`（蓝牙）分别把按键状态映射成 NES 手柄字节，经
  nofrendo 的 `input_update()` 喂入。
- **板级**：`k10.begin()` 拉起 I2C 总线、GPIO 扩展（按键/背光/功放所在）和加速度计；LCD 为提速
  直接驱动，而不走 LVGL canvas。

## 致谢

- NES 模拟：**nofrendo** © Matthew Conte，经 [ducalex/retro-go](https://github.com/ducalex/retro-go)（GPL-2）。
- 蓝牙手柄库：**[K10_Joystick](https://gitcode.com/Xelocity/K10_Joystick)**（`libs/BLE_FFF0/` 抽取自该项目）。
- 配套安卓手柄 App：**[BleJoystick](https://gitcode.com/Xelocity/BleJoystick)**（「自定义蓝牙手柄」模式的手柄端：手机即手柄）。
- 硬件：DFRobot UNIHIKER K10 / DFRobot platform-unihiker。
