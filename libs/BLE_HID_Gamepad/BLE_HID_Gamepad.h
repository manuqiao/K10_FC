#pragma once
/*
 * BLE_HID_Gamepad — UI / lifecycle companion to BLE_HID_Host.
 *
 * Owns the single BLE_HID_Host instance and adds the scan / pick / connect
 * TFT menu plus a thin begin() wrapper. Everything you need to get bonded and
 * subscribed to a standard BLE HID gamepad — i.e. all the steps *before* you
 * decode the reports into your own pad layout.
 *
 * NES-agnostic: it does NOT interpret buttons. Once connected, the app decodes
 * each `{buttons, hat}` report itself by registering a callback on the
 * underlying host (host().onReport(...)).
 *
 * Screen and board buttons are injected at begin(), so this library has no
 * dependency on the application's display or input modules — drop it into any
 * ESP32 + TFT_eSPI project.
 *
 * Requires an ESP32 running the Arduino-ESP32 core and libs/BLE_HID_Host
 * (the BLE Central). One instance only (BLE_HID_Host is a singleton).
 */
#include <Arduino.h>
#include <BLE_HID_Host.h>

class TFT_eSPI;

class BLE_HID_Gamepad {
public:
    // Application-provided board-button reader. Returns an app-defined bitmask;
    // connect_flow() only needs the next/connect/rescan bits (see ButtonMap).
    using ReadButtons = uint8_t (*)();

    // Which bits of the board-button byte mean what in the pick menu. Defaults
    // match the K10 NES layout (board A = next, board B = connect, hold A+B =
    // rescan); pass a custom map at begin() for a different rig. A constructor
    // (not default member initializers) is used so ButtonMap() can serve as a
    // default argument inside this class definition.
    struct ButtonMap {
        uint8_t next;       // cycle to the next device
        uint8_t connect;    // connect to the highlighted device
        uint8_t rescan;     // drop the list and rescan
        ButtonMap(uint8_t n = 0x02, uint8_t c = 0x01, uint8_t r = 0x04)
            : next(n), connect(c), rescan(r) {}
    };

    // Bring up BLE_HID_Host: just-works bonding, drop / auto-reconnect monitor
    // (reconnectTries attempts). The connect menu is driven by `tft` and
    // `boardRead`. Safe to call once.
    void begin(TFT_eSPI& tft, ReadButtons boardRead,
               const char* deviceName = "K10-NES-HID",
               int reconnectTries = 3,
               ButtonMap map = ButtonMap());

    // Scan -> pick -> connect TFT menu loop. Blocks until bonded + subscribed;
    // re-scans on an empty scan or a failed connect. Driven by the injected
    // board buttons. Returns true once paired. Call after begin().
    bool connect_flow();

    // True while the HID link is up.
    bool isConnected() const;

    // The underlying host. Register onReport() / onDisconnect() here from the
    // app to decode reports into your pad layout.
    BLE_HID_Host& host();

private:
    BLE_HID_Host _hid;
    TFT_eSPI*    _tft       = nullptr;
    ReadButtons  _boardRead = nullptr;
    ButtonMap    _map;
};
