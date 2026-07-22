# BLE_HID_Host

一个**屏幕无关**的最简 BLE Central,面向**标准 BLE HID 手柄**(HID 服务 `0x1812`)。
自己调用 `BLEDevice::init()`,开机选模式时与其它 BLE 模式互斥使用。

提供完整链路:**扫描 → 连接 → just-works 配对绑定 → 读 Report Map 自动学布局 →
订阅 Report 特征值 → 解码每帧报告成 {按键位掩码, D-pad 方向}**。不含屏幕、不含按键——
由你自己的 `setup()`/`loop()` 驱动(本工程由 `src/k10_ble_hid.cpp` 驱动)。

## 要求

- 一块跑 **Arduino-ESP32** 核心的 ESP32(任意变体)。`BLEDevice` / Bluedroid 栈随核心
  自带,无需额外安装。
- ESP32-S3 **只支持 BLE**(无经典蓝牙)——这对 BLE HID 手柄正好;走经典蓝牙的 Xbox/PS
  原装手柄仍连不上。
- 每个 sketch 只能有一个实例(Bluedroid 是单例,notify / auth 回调经单个静态实例指针
  路由,见 `BLE_HID_Host::s_inst`)。

---

## 连接流程总览

```
begin(name)            初始化:加密级别 / MTU / SMP just-works / 断线监控任务
   │
scan(ms)               阻塞扫描,收集 {地址, 名字, RSSI, addrType}
   │
connect(i) ───────────────────────────────────────────────────────────┐
   │                                                                   │
   └─► connectAndSubscribe(addr, addrType)                             │
          │                                                            │
          ├─ createClient + connect(addr, addrType)                    │
          │     └─ 因设了加密级别,BLEClient 自动 esp_ble_set_encryption │
          │        → SMP 配对 → onAuthenticationComplete              │
          │                          │                                 │
          ├─ 阻塞等 _authSem (≤5s) ◄──┘  ← 必须认证完成才能动 GATT     │
          │                                                            │
          ├─ setMTU(247) + delay(150)                                  │
          │                                                            │
          └─ discoverAndSubscribe() ──────────────── "连上了"  ◄────────┘
                ├─ 读 Report Map(0x2A4B) → parseReportMap 学布局
                ├─ 订阅所有可通知的 Report 特征值(0x2A4D)
                └─ 把 Protocol Mode(0x2A4E)写成 0(Report 模式)

  之后手柄主动推 notification ──► _notifyCb ──► decodeReport ──► onReport(GamepadState)
```

下面分步说明。行号均指本目录 `BLE_HID_Host.cpp`,应用层引用 `k10_ble_hid.cpp`。

---

## 1. 初始化 `begin()`(`BLE_HID_Host.cpp:108-170`)

按顺序做四件事:

| 步骤 | 代码 | 作用 |
|------|------|------|
| ① | `setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_NO_MITM)` + `setSecurityCallbacks(_secCb)` | 要求 level-2 加密(无 MITM);注册安全回调 |
| ② | `BLEDevice::init(name)` + `setMTU(247)` | 起 Bluedroid;大 MTU 让 Report Map(100-300 字节)能一次读完(默认 23 会截断) |
| ③ | `setCapability(ESP_IO_CAP_NONE)` + `setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND)` + KeySize 16 | **just-works 配对**:无 IO 能力 + Secure Connections + 绑定。密钥写 NVS,**重启后重连免重新配对** |
| ④ | 起 `hid_mon` 监控任务 | 200ms 轮询断线,见第 6 步 |

> SMP 参数调用 `esp_ble_gap_set_security_param`,**必须在 `init()` 之后**(代码里顺序已排好)。

---

## 2. 扫描 `scan(ms)`(`BLE_HID_Host.cpp:172-183`)

阻塞式 active scan。`_HIDScanCb::onResult` 把每个广播设备的 `{address, name, rssi,
addrType}` 去重后塞进 `_devices`。结果通过 `deviceCount()` / `device(i)` 取。

> **addrType(PUBLIC/RANDOM)必须从扫描结果带进 connect**(见「地址类型」)。很多手柄用
> 随机地址,写错会静默连不上。`connect(int index)` 会自动带上扫描到的 addrType。

---

## 3. 连接 + 配对 `connectAndSubscribe()`(`BLE_HID_Host.cpp:230-272`)

这是最容易出问题的一步,严格时序:

```
createClient → client.connect(addr, addrType)
            ↓ (因加密级别)BLEClient 内部自动 esp_ble_set_encryption → SMP 配对
   阻塞等 _authSem(≤5s)  ◄── onAuthenticationComplete 回调 give 该信号量
            ↓ 认证成功后才能动 GATT
   setMTU(247) + delay(150)
            ↓
   discoverAndSubscribe()
```

### 为什么必须等认证完成才订阅

> `registerForNotify()` 自己写 CCCD(0x2902),但**不触发配对**。如果在认证完成前订阅,
> 会拿到 `ESP_GATT_INSUFFICIENT_AUTHENTICATION`,结果一个 report 都收不到。所以这里用
> 一个信号量 `_authSem`,由 `onAuthenticationComplete`(运行在 Bluedroid host task)在配对
> 完成时 give,`connectAndSubscribe` 阻塞在它上面(超时 5s)。源码 `BLE_HID_Host.cpp:255-263`。

---

## 4. 发现服务 + 解析 + 订阅 `discoverAndSubscribe()`(`BLE_HID_Host.cpp:274-375`)

认证通过后做三件事:

### (a) 读 + 解析 Report Map(0x2A4B)—— `parseReportMap()`(`:380-479`)

逐项解析 HID 描述符,记录:按键字段位置(Usage Page `0x09`)、D-pad hat 位置(Usage
`0x39`)、是否有 Report ID、report 总长度。**判别用的是 Usage Page**(按键恒为 0x09,
媒体/键盘为 0x0C/0x07),它能穿透那些把 Consumer Control 放第一个 Application collection、
Game Pad 放第二个的手柄(本手柄正是如此,旧版「只看第一个 collection」的启发式会失败)。

解析失败则退回固定 fallback:`data[0]=按键`,`data[1]&0x0F=hat`(`:554-559`)。

### (b) 订阅所有可通知的 Report 特征值(0x2A4D)—— `:308-361`

```cpp
auto* chars = svc->getCharacteristicsByHandle();   // ★ 必须用 by-handle 版本
for (auto& kv : *chars) {
    BLERemoteCharacteristic* c = kv.second;
    if (!c->getUUID().equals(UUID_REPORT)) continue;     // 只挑 Report(0x2A4D)
    if (c->canNotify() || c->canIndicate())
        c->registerForNotify(_notifyCb, canNotify);      // 库自己写 CCCD
}
```

### (c) 把 Protocol Mode(0x2A4E)写成 `0x00`—— `:364-368`,要求 Report 模式而非 Boot 模式。

---

## 5. 收数据:`_notifyCb` → `decodeReport` → `onReport`

手柄按键时主动推 notification,走静态 trampoline:

```
_notifyCb(BLE_HID_Host.cpp:564-566)
  └─► decodeReport(data, len)              // :493-562
        ├─ [DEBUG] 边沿触发 dump 原始字节 `[hid] rpt len=.. XX XX ..`
        ├─ 按 _layout 抽出 buttons / hat(extractBits)
        ├─ ★ 报文 Report ID 字节缺失特判(见下)
        └─ _reportCb(st)  ──►  应用层 onReport(k10_ble_hid.cpp:143)
                                  └─► BUTTON2NES[] / hatToNES() → g_pad(单字节,NES 掩码)
```

主循环 `loop()` 每帧 `read()` 取 `g_pad`(`k10_ble_hid.cpp:183`)。`g_pad` 是 `volatile
uint8_t`,Xtensa 上单字节存原子,无需锁。

---

## 6. 断线 / 重连

`hid_mon` 监控任务(`BLE_HID_Host.cpp:138-168`)每 200ms 检查:
若 `_discFlag`(由 `_HIDClientCb::onDisconnect` 在 host task 置位)或
`client->isConnected()` 为假 → 清理 client → 调 `onDisconnect`(应用层清 `g_pad`
防按键卡住,`k10_ble_hid.cpp:165-168`)→ 按 `setAutoReconnect(tries)` 用缓存的地址
重连(本工程设 3 次)。

---

## 踩过的两个关键坑(配「连上了但没按键」必看)

### 坑 1:必须用 `getCharacteristicsByHandle()`,不能用 `getCharacteristics()`

> `getCharacteristics()` 返回 `std::map`,**以 UUID 字符串为 key**,而
> `std::map::insert` 对重复 key 是静默 no-op。标准 HID-over-GATT 里每个 Report ID 一个
> Report char、共享 UUID `0x2A4D`,于是两个 char 在 map 里塌成一个——第二个被丢掉。
> 结果:只留下第一个(比如消费控制 rid=3),手柄报告 char(rid=4)被隐藏,它的报告永远
> 收不到。**这正是「连接成功但按键日志为空」的根因。** `getCharacteristicsByHandle()`
> 以唯一属性句柄为 key,每个特征值都能存活。源码注释 `:318-328`。

### 坑 2:订阅所有 Report char,不要按 Report ID 过滤

> 便宜手柄的 Report Reference(0x2908)描述符经常乱标,过滤会把正确的那个漏掉。改为
> 订阅**每个**可通知/可指示的 Report char;别的 ID 的报告会在 `decodeReport()` 里被
> 丢弃,零成本且绝不可能错。源码注释 `:308-317`。

### 特判:描述符声明了 Report ID,但报文不带 ID 字节

> 本手柄的描述符声明 Report ID,理应每个报文首字节是 ID,但**实际发来的报文不带这个
> 首字节**(声明 11 字节、实发 10 字节)。旧逻辑按 `data[0]==reportId` 判断,而
> `data[0]` 其实是某轴值 0x80,于是每个报文都被丢、永远解不出按键。`decodeReport`
> 改用**长度**判断(`:522-546`):`len==reportLen` 才认 ID 字节存在并校验;`len==
> reportLen-1` 视为手柄省略了 ID 字节,直接用整个 payload。

---

## API

| 方法 | 说明 |
|------|------|
| `begin(name)` | 初始化 BLE + 配置 just-works 配对 + 起断线监控任务。幂等。 |
| `scan(ms)` | 阻塞扫描。结果经 `deviceCount()`/`device()` 取。 |
| `device(i)` / `deviceCount()` | 扫描结果(含 name/address/rssi/addrType)。 |
| `connect(i)` | 按扫描序号连接(自动带扫描到的 addrType)。 |
| `connect(addr, type)` | 按地址连接;`type` 传扫描观察到的 addrType。 |
| `disconnect()` / `isConnected()` | |
| `setAutoReconnect(tries)` | `0`=关;断线后自动重试次数。 |
| `onReport(cb)` | 解码后的 `{buttons, hat}`(Bluedroid task——要快)。 |
| `onDisconnect(cb)` | 断线后、自动重连前触发。 |
| `onLog(cb)` | 诊断输出;默认 `Serial`。 |
| `layout()` | 最后一次解析出的 `HidLayout`(调试未知手柄用)。 |

`GamepadState`:`buttons` 的 bit i(0 基)即第 i+1 个 HID 按键被按;`hat` 为 0=N、
1=NE、2=E、…7=NW,8..15/0xFF=松开。

---

## 调试未知手柄

廉价手柄按键编号不统一,映射表 `BUTTON2NES[]`(`k10_ble_hid.cpp:52-69`)**是唯一的调试点**:

1. 把 `BLE_HID_Host.cpp` 顶部 `K10_BLE_HID_DEBUG=1`(`:39-41`)——串口会逐帧边沿触发打印
   原始字节 `[hid] rpt len=.. XX XX ..`(报告是否到达、Report ID 是几,一目了然)。
2. 应用层 `k10_ble_hid.cpp` 顶部 `K10_BLE_HID_PRINT=1`(`:98-100`)——打印
   `[hid] btn=0x.. held=[1,3] hat=2 -> nes=0x..`(原始按键号、hat、解码结果)。
3. 按一下每个键,读 `held=[N]` 里的 1 基按键号,照着填 `BUTTON2NES[]`。
4. 调好后把两个开关都置 0 静音。

`onReport()`(`k10_ble_hid.cpp:143-163`)、`hatToNES()`(`:73-87`)是应用层,不属于本库。
