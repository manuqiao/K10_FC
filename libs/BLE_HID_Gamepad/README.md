# BLE_HID_Gamepad

UI / lifecycle companion to [`BLE_HID_Host`](../BLE_HID_Host)。拥有那一个
`BLE_HID_Host` 实例,并加上**扫描 / 选择 / 连接**的 TFT 菜单 + 一个极薄的 `begin()`
封装——即把标准 BLE HID 手柄**配对绑定并订阅好**之前的全部步骤打包成库。

**NES 无关**:本库不解释按键。连上之后,应用自己解码每帧 `{buttons, hat}` 报告
(在底层 host 上注册回调 `host().onReport(...)`)。所以"按键映射"留在应用层,本库只管
"映射之前"的链路 + 选单。

屏幕与板载按键由 `begin()` **注入**,因此本库不依赖应用的显示/输入模块——任何
ESP32 + TFT_eSPI 工程都能直接用。

## 要求

- 一块跑 **Arduino-ESP32** 核心的 ESP32(任意变体)。
- 同目录上一层的 `BLE_HID_Host`(BLE Central,负责扫描/连接/配对/订阅/解码)。
- 一块 TFT_eSPI 屏 + 1~3 个板载按键(菜单用:下一个 / 连接 / 重扫)。
- 每个 sketch 只能有一个实例(`BLE_HID_Host` 是单例)。

## 用法

```cpp
#include <TFT_eSPI.h>
#include <BLE_HID_Gamepad.h>

TFT_eSPI tft;
BLE_HID_Gamepad pad;

// 应用层:把 host 解码出的 {buttons, hat} 变成你自己的按键布局。
static volatile uint8_t g_pad = 0;
static void onReport(const BLE_HID_Host::GamepadState& st) {
    uint8_t v = 0;
    if (st.buttons & 0x0001) v |= 0x01;   // 你的 Button1 -> 你的 bit0
    // ... hat -> 方向 ...
    g_pad = v;
}

// 你的板载按键读取器,返回一个位掩码(哪几位代表 next/connect/rescan 自定)。
uint8_t readBoardButtons() { /* ... */ }

void setup() {
    tft.begin();

    pad.begin(tft, readBoardButtons);          // BLE 初始化 + just-works 配对 + 断线监控
    pad.host().onReport(onReport);             // 注册你的解码回调
    pad.host().onDisconnect([] { g_pad = 0; }); // 断线时清状态

    if (!pad.connect_flow()) {                 // 扫描 -> 选 -> 连,阻塞直到配对成功
        // 连不上……
    }
}

void loop() {
    uint8_t p = g_pad;                         // 应用自己读映射结果
    // ...
}
```

## API

| 方法 | 说明 |
|------|------|
| `begin(tft, boardRead, name?, reconnect?, map?)` | 拥有 `BLE_HID_Host`;初始化 BLE、just-works 配对、起断线 / 自动重连(`reconnect` 次)监控任务。`tft` + `boardRead` 驱动选单。 |
| `connect_flow()` | 扫描 → 选择 → 连接 的 TFT 菜单循环。阻塞直到绑定 + 订阅成功;空扫或连接失败自动重扫。返回是否配对成功。 |
| `isConnected()` | HID 链路是否在线。 |
| `host()` | 底层 `BLE_HID_Host&`。在此注册 `onReport()` / `onDisconnect()` 做你自己的按键解码。 |

### `ButtonMap`

板载按键字节里哪几位代表"下一个 / 连接 / 重扫"。默认对齐 K10 NES 布局:

| 字段 | 默认 | 含义 |
|------|------|------|
| `next`    | `0x02` | 板载 A:循环到下一个设备 |
| `connect` | `0x01` | 板载 B:连接高亮设备 |
| `rescan`  | `0x04` | 同时按住 A+B:丢弃列表重扫 |

按键不同就传一个自定义 `ButtonMap` 给 `begin()`。

## 分层

```
BLE_HID_Host        原始 BLE:扫描/连接/配对/订阅/解码 -> {buttons, hat}
   ▲
   │ 拥有
BLE_HID_Gamepad     + 扫描/选择/连接 TFT 菜单 + begin() 封装  (本库,NES 无关)
   ▲
   │ host().onReport(...)
应用层 (k10_ble_hid)  BUTTON2NES[] / hatToNES() -> NES 掩码 g_pad
```

调试未知手柄的原始按键号,见 `BLE_HID_Host` 的 README(`K10_BLE_HID_DEBUG` 逐帧 dump
`[hid] rpt ...`);应用层的解码行 `[hid] btn=... held=[...] hat=.. -> nes=0x..` 由应用
自己打印。
