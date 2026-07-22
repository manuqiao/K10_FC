#include "BLE_HID_Gamepad.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

// ============================================================================
//  Scan / pick / connect UI (TFT_eSPI, driven by the injected board buttons)
// ============================================================================
//  Ported from the former src/k10_ble_hid.cpp: the screen + button plumbing is
//  now injected (this->*_tft / this->_boardRead), so the library has no
//  dependency on the app's display/input modules.
static const int DISP_W   = 320;
static const int DISP_H   = 240;
static const int TITLE_Y  = 6;
static const int SEP_Y    = 28;
static const int TOP_Y    = 40;
static const int ROW_H    = 22;
static const int MAX_ROWS = 7;
static const int FOOTER_Y = DISP_H - 20;

static void drawScanning(TFT_eSPI &tft)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  BLE HID Scanning...", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Searching for HID gamepads (2s)", 8, TOP_Y + 10);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("See serial for live scan results", 8, TOP_Y + 40);
}

// `order` maps a display row -> hid.device() index (sorted by RSSI desc).
static void drawPicker(TFT_eSPI &tft, BLE_HID_Host &hid,
                       const int *order, int count, int sel, int top)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextWrap(false);
    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  Select HID Device", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);

    int rows = count < MAX_ROWS ? count : MAX_ROWS;
    for (int i = 0; i < rows; i++)
    {
        int idx = top + i;
        if (idx >= count) break;
        int y = TOP_Y + i * ROW_H;
        const BLE_HID_Host::Device *d = hid.device(order[idx]);
        bool hot = (idx == sel);

        if (hot)
        {
            tft.fillRect(0, y - 1, DISP_W, ROW_H, TFT_NAVY);
            tft.setTextColor(TFT_YELLOW, TFT_NAVY);
            tft.drawString(">", 6, y + 3);
        }
        else
        {
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const char *nm = (d && d->name.length()) ? d->name.c_str()
                                                 : (d ? d->address.c_str() : "?");
        char line[48];
        snprintf(line, sizeof(line), "%-22s %ddB", nm, d ? d->rssi : 0);
        tft.drawString(line, 24, y + 3);
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("A:next  B:connect  hold A+B:rescan", 8, FOOTER_Y);
}

static void drawConnecting(TFT_eSPI &tft, const char *name)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Connecting (pairing)...", 8, TOP_Y + 10);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(name, 8, TOP_Y + 40);
}

static void drawMessage(TFT_eSPI &tft, uint16_t color,
                        const char *l1, const char *l2)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(l1, 8, TOP_Y + 10);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(l2, 8, TOP_Y + 40);
}

// ============================================================================
//  BLE_HID_Gamepad
// ============================================================================
void BLE_HID_Gamepad::begin(TFT_eSPI& tft, ReadButtons boardRead,
                            const char* deviceName, int reconnectTries,
                            ButtonMap map)
{
    _tft       = &tft;
    _boardRead = boardRead;
    _map       = map;

    // onLog() defaults to Serial inside BLE_HID_Host, so diagnostics still land
    // on the serial line without the app wiring it up.
    _hid.begin(deviceName);
    _hid.setAutoReconnect(reconnectTries);
}

bool BLE_HID_Gamepad::isConnected() const
{
    return _hid.isConnected();
}

BLE_HID_Host& BLE_HID_Gamepad::host()
{
    return _hid;
}

bool BLE_HID_Gamepad::connect_flow()
{
    TFT_eSPI &tft = *_tft;

    for (;;)
    {
        // ---- scan (blocking 2 s) ----
        drawScanning(tft);
        _hid.scan(2000);

        int count = _hid.deviceCount();
        if (count == 0)
        {
            drawMessage(tft, TFT_RED, "No BLE devices found", "Rescanning in 2s...");
            delay(2000);
            continue;
        }

        // Sort device indices by RSSI (strongest first). Cap the list so a noisy
        // 2.4 GHz environment doesn't overflow the stack array.
        int order[32];
        if (count > 32) count = 32;
        for (int i = 0; i < count; i++) order[i] = i;
        for (int i = 0; i < count - 1; i++)
            for (int j = i + 1; j < count; j++)
                if (_hid.device(order[j])->rssi > _hid.device(order[i])->rssi)
                { int t = order[i]; order[i] = order[j]; order[j] = t; }

        // ---- pick (injected board buttons) ----
        int sel = 0, top = 0;
        drawPicker(tft, _hid, order, count, sel, top);
        uint8_t prev = _boardRead();

        int chosen = -1;          // _hid.device() index to connect to
        for (;;)
        {
            uint8_t cur = _boardRead();
            bool next_edge   = (cur & _map.next)    && !(prev & _map.next);
            bool conn_edge   = (cur & _map.connect) && !(prev & _map.connect);
            bool rescan_edge = (cur & _map.rescan)  && !(prev & _map.rescan);
            prev = cur;

            if (rescan_edge) break;   // -> outer loop rescans

            if (next_edge && count > 1)
            {
                sel = (sel + 1) % count;
                if (sel < top) top = sel;
                else if (sel >= top + MAX_ROWS) top = sel - MAX_ROWS + 1;
                drawPicker(tft, _hid, order, count, sel, top);
            }
            if (conn_edge) { chosen = order[sel]; break; }

            delay(20);
        }
        if (chosen < 0) { delay(150); continue; }   // rescan requested

        // ---- connect (bonds + subscribes; may take a couple seconds) ----
        const BLE_HID_Host::Device *d = _hid.device(chosen);
        const char *name = (d && d->name.length()) ? d->name.c_str()
                         : (d ? d->address.c_str() : "?");
        drawConnecting(tft, name);

        if (_hid.connect(chosen))
        {
            drawMessage(tft, TFT_GREEN, "Connected:", name);
            delay(700);
            return true;
        }
        drawMessage(tft, TFT_RED, "Connect failed", "Retrying scan in 2s...");
        delay(2000);
        // loop -> fresh scan
    }
}
