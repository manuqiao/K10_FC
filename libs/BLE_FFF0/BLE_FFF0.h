#pragma once
/*
 * BLE_FFF0 — a minimal, UI-agnostic BLE Central for HM-10-style transparent-UART
 * peripherals (service 0xFFF0, characteristic 0xFFF1). Provides scan / connect /
 * receive (via notifications) / write. No screen, no buttons — drop it into any
 * ESP32 project and drive it from your own setup()/loop().
 *
 * Extracted from the K10 joystick FFF0/FFF1 monitor; the screen/UI code was
 * stripped out and the BLE plumbing packaged as a reusable class.
 *
 * Requires an ESP32 running the Arduino-ESP32 core (BLEDevice / Bluedroid stack).
 * ESP32-S3 is BLE-only (no Bluetooth Classic).
 *
 * Threading:
 *   - onData()        fires on the Bluedroid host task — keep it short, or
 *                     queue the bytes and process them in your loop().
 *   - onDisconnect()  fires on a background monitor task.
 *   - scan()/connect()/write() are safe to call from your loop() task.
 *
 * One instance only: the underlying Bluedroid stack is a singleton, so the
 * notification callback is routed through a single static instance pointer.
 */
#include <Arduino.h>
#include <functional>
#include <vector>

#include <esp_gap_ble_api.h>   // esp_ble_addr_type_t

// Forward declarations so the public header doesn't pull in the full BLE headers.
class BLEClient;
class BLERemoteCharacteristic;
class _FFF0ScanCb;
class _FFF0ClientCb;

class BLE_FFF0 {
public:
    struct Device {
        String address;                  // "aa:bb:cc:dd:ee:ff"
        String name;
        int8_t rssi;
        esp_ble_addr_type_t addrType;    // PUBLIC or RANDOM (matters for connect)
    };

    using DataCallback       = std::function<void(const uint8_t* data, size_t len)>;
    using DisconnectCallback = std::function<void()>;
    using LogCallback        = std::function<void(const String&)>;

    // ---- lifecycle ---------------------------------------------------------
    // Call once in setup(). Spawns a background monitor task that detects drops
    // and (if enabled) auto-reconnects.
    void begin(const char* deviceName = "BLE-FFF0");

    // ---- scan --------------------------------------------------------------
    // Blocking scan. Results available via deviceCount()/device() afterwards.
    void scan(uint32_t durationMs = 8000);
    void clearDevices();
    int  deviceCount() const;
    const Device* device(int index) const;                  // nullptr if out of range
    const Device* find(const String& nameOrAddrSub) const;  // case-insensitive substring

    // ---- connect -----------------------------------------------------------
    // By scan index — addrType is taken from the scan result automatically.
    bool connect(int deviceIndex);
    // By address string. Pass the addrType seen during scan; phone peripherals
    // usually advertise a RANDOM address and would silently fail with the PUBLIC default.
    bool connect(const String& address,
                 esp_ble_addr_type_t addrType = BLE_ADDR_TYPE_PUBLIC);
    void disconnect();
    bool isConnected() const;

    // Reconnect on an unexpected drop. 0 = off (default). tries = attempts.
    void setAutoReconnect(int tries) { _autoReconnect = tries; }

    // ---- I/O + callbacks ---------------------------------------------------
    void onData(DataCallback cb)             { _dataCb = cb; }
    void onDisconnect(DisconnectCallback cb) { _discCb  = cb; }
    void onLog(LogCallback cb)               { _logCb   = cb; }

    // Write to FFF1. Returns false if not connected or FFF1 not writable.
    bool write(const uint8_t* data, size_t len);
    // Parse and write hex bytes, e.g. writeHex("01 02 03 AA"). Returns bytes written or -1.
    int  writeHex(const String& hex);

private:
    friend class _FFF0ScanCb;
    friend class _FFF0ClientCb;

    // Notify trampoline (static — Bluedroid's notify callback has no user-data slot).
    static void _notifyCb(BLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify);

    void log(const char* fmt, ...);
    void _onScanResult(const String& addr, const String& name,
                       int8_t rssi, esp_ble_addr_type_t addrType);
    bool connectAndSubscribe(const String& addr, esp_ble_addr_type_t addrType);
    static void monitorTask(void* arg);

    std::vector<Device> _devices;
    DataCallback        _dataCb;
    DisconnectCallback  _discCb;
    LogCallback         _logCb;

    BLEClient*              _client = nullptr;
    BLERemoteCharacteristic* _fff1  = nullptr;
    _FFF0ScanCb*            _scanCb   = nullptr;
    _FFF0ClientCb*          _clientCb = nullptr;

    String               _lastAddr;
    esp_ble_addr_type_t  _lastAddrType = BLE_ADDR_TYPE_PUBLIC;

    int           _autoReconnect = 0;
    volatile bool _connected = false;   // we believe the link is up
    volatile bool _discFlag  = false;   // set by onDisconnect (host task)
    SemaphoreHandle_t _mtx = nullptr;   // guards _client / _fff1 / connect+write lifecycle
};
