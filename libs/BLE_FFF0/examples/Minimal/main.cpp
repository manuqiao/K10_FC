/*
 * BLE_FFF0 — minimal serial example (no screen).
 *
 * Flow:
 *   1. Scan 8s, print the device list.
 *   2. Pick a device via serial "<n>" (or send "r" to rescan).
 *   3. Connect, subscribe, and print every FFF1 notification as hex.
 *   4. While connected:
 *        "w 01 02 03"  -> write hex bytes to FFF1
 *        "q"           -> disconnect + rescan
 *
 * Copy this into a project's src/main.cpp (or open the folder in Arduino IDE).
 */
#include <Arduino.h>
#include <BLE_FFF0.h>

BLE_FFF0 ble;

static void printList() {
    Serial.printf("[list] %d device(s). Send \"<n>\" to connect, \"r\" to rescan.\n",
                  ble.deviceCount());
    for (int i = 0; i < ble.deviceCount(); i++) {
        const auto* d = ble.device(i);
        Serial.printf("  [%d] %-20s %s  rssi=%d  %s\n", i,
                      d->name.length() ? d->name.c_str() : "(unnamed)",
                      d->address.c_str(), d->rssi,
                      d->addrType == BLE_ADDR_TYPE_RANDOM ? "RANDOM" : "PUBLIC");
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);

    ble.onLog([](const String& s) { Serial.println(s); });
    ble.onData([](const uint8_t* d, size_t n) {
        Serial.print("[rx] ");
        for (size_t i = 0; i < n; i++) Serial.printf("%02X ", d[i]);
        Serial.println();
    });
    ble.onDisconnect([]() { Serial.println("[dropped] link lost"); });

    ble.begin("BLE-FFF0-Example");
    ble.setAutoReconnect(3);

    Serial.println("scanning 8s...");
    ble.scan(8000);
    printList();
}

static char serBuf[64];
static size_t serLen = 0;
static bool readLine() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { serBuf[serLen] = 0; serLen = 0; return true; }
        if (serLen < sizeof(serBuf) - 1) serBuf[serLen++] = c;
    }
    return false;
}

void loop() {
    if (!ble.isConnected()) return;   // idle until you pick a device

    if (readLine() && serBuf[0]) {
        if (serBuf[0] == 'q') {
            ble.disconnect();
        } else if (serBuf[0] == 'w' || serBuf[0] == 'W') {
            String hex = String(serBuf).substring(1);
            int n = ble.writeHex(hex);
            Serial.printf("[tx] %d bytes\n", n);
        }
    }
}
