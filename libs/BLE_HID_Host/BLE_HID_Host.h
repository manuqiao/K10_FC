#pragma once
/*
 * BLE_HID_Host — a minimal, UI-agnostic BLE Central for **standard BLE HID
 * gamepads** (HID service 0x1812). Connects, bonds (just-works), subscribes to
 * the gamepad's Report characteristic (0x2A4D), parses the HID Report Map
 * (0x2A4B) to learn the report layout, and decodes each incoming report into a
 * small gamepad state (button bitmask + D-pad hat).
 *
 * Calls BLEDevice::init() itself; boot control-mode selection ensures it is
 * never used together with another BLE mode.
 *
 * Requires an ESP32 running the Arduino-ESP32 core (BLEDevice / Bluedroid).
 * ESP32-S3 is BLE-only (no Bluetooth Classic) — that's fine, BLE HID gamepads
 * are the target. Bonding keys are persisted to NVS by the stack, so a
 * reconnect after reboot re-encrypts without re-pairing.
 *
 * Threading:
 *   - onReport()        fires on the Bluedroid host task — keep it short.
 *   - onDisconnect()    fires on the background monitor task.
 *   - scan()/connect()  safe to call from your loop() task.
 *
 * One instance only: the Bluedroid stack is a singleton, so the notify callback
 * is routed through a single static instance pointer.
 */
#include <Arduino.h>
#include <functional>
#include <vector>

#include <esp_gap_ble_api.h>   // esp_ble_addr_type_t

// Forward declarations so the public header doesn't pull in the full BLE headers.
class BLEClient;
class BLERemoteCharacteristic;
class _HIDScanCb;
class _HIDClientCb;
class _HIDSecCb;

class BLE_HID_Host {
public:
    struct Device {
        String address;                  // "aa:bb:cc:dd:ee:ff"
        String name;
        int8_t rssi;
        esp_ble_addr_type_t addrType;    // PUBLIC or RANDOM (matters for connect)
    };

    // Decoded report layout, learned from the Report Map descriptor. Exposed for
    // serial logging / debugging unknown pads. `valid==false` means auto-detect
    // failed and the fixed-layout fallback is in effect.
    struct HidLayout {
        bool     valid        = false;
        bool     hasReportId  = false;   // descriptor declared any REPORT_ID?
        uint8_t  reportId     = 0;       // gamepad report's ID (0 if none)
        uint16_t btnBitOffset = 0;       // bit offset of the button field
        uint8_t  btnBitCount  = 0;       // number of button bits
        bool     hasHat       = false;   // D-pad hat (Usage 0x39) present?
        uint16_t hatBitOffset = 0;
        uint8_t  hatBits      = 0;       // usually 4
        uint16_t reportLen    = 0;       // expected report byte length
    };

    // Parsed per-report state, pushed to the onReport callback.
    //   buttons: bit i (0-based) == Button (i+1) held. Up to 32 buttons.
    //   hat:     0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW; 8..15 / 0xFF = released.
    struct GamepadState {
        uint32_t buttons = 0;
        uint8_t  hat     = 0xFF;         // 0xFF => no hat / released
    };

    using ReportCallback     = std::function<void(const GamepadState&)>;
    using DisconnectCallback = std::function<void()>;
    using LogCallback        = std::function<void(const String&)>;

    // ---- lifecycle ---------------------------------------------------------
    // Call once in setup(). Configures just-works bonding (ESP_IO_CAP_NONE +
    // SC_BOND), sets the encryption level so BLEClient auto-encrypts on connect,
    // and spawns the drop/reconnect monitor task.
    void begin(const char* deviceName = "BLE-HID");

    // ---- scan --------------------------------------------------------------
    // Blocking scan. Results available via deviceCount()/device() afterwards.
    void scan(uint32_t durationMs = 8000);
    void clearDevices();
    int  deviceCount() const;
    const Device* device(int index) const;                  // nullptr if out of range

    // ---- connect -----------------------------------------------------------
    // By scan index — addrType is taken from the scan result automatically.
    // Bonds + subscribes to the gamepad Report char. Returns false on connect
    // failure, auth timeout, or no HID service.
    bool connect(int deviceIndex);
    // By address string. Pass the addrType seen during scan.
    bool connect(const String& address,
                 esp_ble_addr_type_t addrType = BLE_ADDR_TYPE_PUBLIC);
    void disconnect();
    bool isConnected() const;

    // Reconnect on an unexpected drop. 0 = off (default). tries = attempts.
    void setAutoReconnect(int tries) { _autoReconnect = tries; }

    // ---- I/O + callbacks ---------------------------------------------------
    void onReport(ReportCallback cb)             { _reportCb = cb; }
    void onDisconnect(DisconnectCallback cb)     { _discCb   = cb; }
    void onLog(LogCallback cb)                   { _logCb    = cb; }

    // Last parsed Report Map layout (for logging / debugging).
    const HidLayout& layout() const { return _layout; }

private:
    friend class _HIDScanCb;
    friend class _HIDClientCb;
    friend class _HIDSecCb;

    // Notify trampoline (static — Bluedroid's notify callback has no user-data slot).
    static void _notifyCb(BLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify);

    void log(const char* fmt, ...);
    void _onScanResult(const String& addr, const String& name,
                       int8_t rssi, esp_ble_addr_type_t addrType);
    bool connectAndSubscribe(const String& addr, esp_ble_addr_type_t addrType);
    bool discoverAndSubscribe();   // post-auth: service 0x1812, Report Map, Report chars
    void parseReportMap(const uint8_t* data, size_t len);
    void decodeReport(const uint8_t* data, size_t len);

    std::vector<Device> _devices;
    ReportCallback      _reportCb;
    DisconnectCallback  _discCb;
    LogCallback         _logCb;

    BLEClient*              _client = nullptr;
    _HIDScanCb*             _scanCb   = nullptr;
    _HIDClientCb*           _clientCb = nullptr;
    _HIDSecCb*              _secCb    = nullptr;

    String               _lastAddr;
    esp_ble_addr_type_t  _lastAddrType = BLE_ADDR_TYPE_PUBLIC;

    HidLayout _layout;

    int           _autoReconnect = 0;
    volatile bool _connected = false;   // we believe the link is up
    volatile bool _discFlag  = false;   // set by onDisconnect (host task)
    volatile bool _authOk    = false;   // set by onAuthenticationComplete (host task)
    SemaphoreHandle_t _mtx       = nullptr;  // guards _client lifecycle
    SemaphoreHandle_t _authSem   = nullptr;  // given by onAuthenticationComplete

    // Called from the security callback (host task): stash auth outcome + unblock
    // connectAndSubscribe()'s wait on _authSem.
    void _authDone(bool ok);

    static BLE_HID_Host* s_inst;        // single-instance router for notify/auth
};
