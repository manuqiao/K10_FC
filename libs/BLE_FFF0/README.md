# BLE_FFF0

A minimal, **screen-agnostic** BLE Central for HM-10-style transparent-UART
peripherals (service `0xFFF0`, characteristic `0xFFF1`). Extracted from the
K10 joystick monitor so it can be dropped into any ESP32 project.

Provides: **scan → connect → receive (notifications) → write**. No screen, no
buttons — you drive it from your own `setup()`/`loop()`.

## Requirements

- An ESP32 (any variant) running the **Arduino-ESP32** core. The `BLEDevice` /
  Bluedroid stack ships with the core — no extra library to install.
- ESP32-S3 is **BLE-only** (no Bluetooth Classic).
- One instance per sketch (Bluedroid is a singleton; the notify callback is
  routed through a single static instance pointer).

## Install

Copy the whole `BLE_FFF0/` folder into your project's `lib/` directory
(PlatformIO auto-discovers it). For Arduino IDE, drop it into `libraries/`.

## Usage

```cpp
#include <BLE_FFF0.h>

BLE_FFF0 ble;

void setup() {
    Serial.begin(115200);

    ble.onLog([](const String& s) { Serial.println(s); });
    ble.onData([](const uint8_t* d, size_t n) {
        // runs on the Bluedroid task — keep it short
        Serial.print("[rx] ");
        for (size_t i = 0; i < n; i++) { Serial.printf("%02X ", d[i]); }
        Serial.println();
    });
    ble.onDisconnect([]() { Serial.println("[dropped]"); });

    ble.begin("MyCentral");
    ble.setAutoReconnect(3);          // optional: retry up to 3x on a drop

    Serial.println("scanning...");
    ble.scan(8000);                   // blocking
    for (int i = 0; i < ble.deviceCount(); i++) {
        const auto* d = ble.device(i);
        Serial.printf("  [%d] %-20s %s  %ddB\n",
                      i, d->name.c_str(), d->address.c_str(), d->rssi);
    }

    const auto* t = ble.find("HMSoft");        // match by name OR address
    if (t) ble.connect(t->address, t->addrType);  // addrType from the scan result
}

void loop() {
    if (ble.isConnected()) {
        ble.writeHex("AT+VERSION\r\n");        // example write
        delay(1000);
    }
}
```

### Address type matters

Phone peripherals (and many random devices) advertise a **RANDOM** BLE address.
`BLEClient::connect()` defaults to **PUBLIC** and will silently fail to connect
to them. `connect(int index)` reads `addrType` from the scan result for you; if
you call `connect(address, ...)` yourself, pass the type you observed:

```cpp
const auto* d = ble.find("MyDevice");
ble.connect(d->address, d->addrType);   // not the PUBLIC default
```

## API

| Method | Notes |
|--------|-------|
| `begin(name)` | Init BLE + start the disconnect monitor task. |
| `scan(ms)` | Blocking scan. Results via `deviceCount()`/`device()`. |
| `device(i)` / `find(sub)` / `deviceCount()` | `find()` is case-insensitive on name **and** address. |
| `connect(i)` | Connect by scan index (uses scanned addrType). |
| `connect(addr, type)` | Connect by address; pass the scanned addrType. |
| `disconnect()` / `isConnected()` | |
| `setAutoReconnect(tries)` | `0` = off. Auto-retry on an unexpected drop. |
| `write(bytes, len)` / `writeHex("01 02")` | Write to FFF1. |
| `onData(cb)` | Notification payload (Bluedroid task — be quick). |
| `onDisconnect(cb)` | Fires after a drop, before any auto-reconnect. |
| `onLog(cb)` | Capture diagnostics; defaults to `Serial`. |

## FFF1 fallbacks

If the peripheral doesn't expose a clean 16-bit `0xFFF1`, the library degrades
gracefully, in order:

1. `0xFFF0` service present → match `0xFFF1`; else match a 128-bit UUID whose
   string contains `fff1`.
2. Else subscribe to the **first notifiable** characteristic in `0xFFF0`.
3. No `0xFFF0` at all → **probe mode**: subscribe to every notifiable
   characteristic on the device.

Writes always target the resolved FFF1 characteristic (if any).
