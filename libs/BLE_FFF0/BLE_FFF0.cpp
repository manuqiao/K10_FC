/*
 * BLE_FFF0 — implementation. See BLE_FFF0.h for the public surface.
 *
 * Logic ported from the K10 joystick monitor's connect/subscribe path; the
 * screen rendering and button handling were removed.
 */
#include "BLE_FFF0.h"

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>

// HM-10-style transparent-UART service/characteristic.
static const BLEUUID UUID_UART_SERVICE = BLEUUID((uint16_t)0xFFF0);
static const BLEUUID UUID_UART_CHAR    = BLEUUID((uint16_t)0xFFF1);

// Single-instance pointer used to route the no-user-data notify callback.
static BLE_FFF0* s_inst = nullptr;

// ============================================================================
//  Scan callbacks
// ============================================================================
class _FFF0ScanCb : public BLEAdvertisedDeviceCallbacks {
    BLE_FFF0* _o;
public:
    explicit _FFF0ScanCb(BLE_FFF0* o) : _o(o) {}

    void onResult(BLEAdvertisedDevice d) override {
        std::string name = d.getName();
        std::string addr = d.getAddress().toString();
        esp_ble_addr_type_t at = d.getAddressType();

        _o->log("[FFF0 scan] '%s' %s rssi=%d addrType=%s",
                name.empty() ? "(no name)" : name.c_str(),
                addr.c_str(), d.getRSSI(),
                at == BLE_ADDR_TYPE_RANDOM ? "RANDOM" : "PUBLIC");

        _o->_onScanResult(String(addr.c_str()),
                          String(name.c_str()),
                          (int8_t)d.getRSSI(), at);
    }
};

// ============================================================================
//  Client (connect/disconnect) callbacks
// ============================================================================
class _FFF0ClientCb : public BLEClientCallbacks {
    BLE_FFF0* _o;
public:
    explicit _FFF0ClientCb(BLE_FFF0* o) : _o(o) {}
    void onConnect(BLEClient*) override {}
    void onDisconnect(BLEClient*) override {
        // Runs on the Bluedroid host task — never touch _client here. Just flag.
        _o->_discFlag = true;
        _o->log("[FFF0] disconnected");
    }
};

// ============================================================================
//  Public API
// ============================================================================
void BLE_FFF0::begin(const char* deviceName) {
    if (!_mtx) _mtx = xSemaphoreCreateMutex();
    s_inst = this;
    if (!_scanCb)   _scanCb   = new _FFF0ScanCb(this);
    if (!_clientCb) _clientCb = new _FFF0ClientCb(this);
    BLEDevice::init(deviceName ? deviceName : "BLE-FFF0");

    static bool taskStarted = false;
    if (!taskStarted) {
        taskStarted = true;
        xTaskCreate(monitorTask, "fff0_mon", 4096, this, 1, nullptr);
    }
}

void BLE_FFF0::scan(uint32_t durationMs) {
    _devices.clear();
    BLEScan* s = BLEDevice::getScan();
    s->setAdvertisedDeviceCallbacks(_scanCb, false);
    s->setActiveScan(true);
    s->setInterval(80);
    s->setWindow(60);
    uint32_t secs = durationMs / 1000; if (secs == 0) secs = 1;
    log("[FFF0] scanning %us...", (unsigned)secs);
    s->start(secs);   // blocking — returns when the scan window ends
    log("[FFF0] scan done: %d device(s)", (int)_devices.size());
}

void BLE_FFF0::clearDevices() { _devices.clear(); }
int  BLE_FFF0::deviceCount() const { return (int)_devices.size(); }

const BLE_FFF0::Device* BLE_FFF0::device(int index) const {
    if (index < 0 || index >= (int)_devices.size()) return nullptr;
    return &_devices[(size_t)index];
}

const BLE_FFF0::Device* BLE_FFF0::find(const String& sub) const {
    if (sub.length() == 0) return nullptr;
    String needle = sub; needle.toLowerCase();
    for (const auto& d : _devices) {
        String n = d.name;    n.toLowerCase();
        String a = d.address; a.toLowerCase();
        if (n.indexOf(needle) >= 0 || a.indexOf(needle) >= 0) return &d;
    }
    return nullptr;
}

bool BLE_FFF0::connect(int deviceIndex) {
    const Device* d = device(deviceIndex);
    if (!d) { log("[FFF0] connect: bad index %d", deviceIndex); return false; }
    return connect(d->address, d->addrType);
}

bool BLE_FFF0::connect(const String& address, esp_ble_addr_type_t addrType) {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    bool ok = connectAndSubscribe(address, addrType);
    _connected = ok;
    xSemaphoreGive(_mtx);
    return ok;
}

void BLE_FFF0::disconnect() {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    _connected = false;
    _discFlag  = false;
    if (_client) { _client->disconnect(); delete _client; _client = nullptr; }
    _fff1 = nullptr;
    xSemaphoreGive(_mtx);
    log("[FFF0] disconnected on request");
}

bool BLE_FFF0::isConnected() const { return _connected; }

bool BLE_FFF0::write(const uint8_t* data, size_t len) {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    bool ok = false;
    if (_client && _fff1) {
        if (_fff1->canWrite() || _fff1->canWriteNoResponse()) {
            _fff1->writeValue(const_cast<uint8_t*>(data), len, _fff1->canWrite());
            ok = true;
        } else {
            log("[FFF0] FFF1 not writable");
        }
    } else {
        log("[FFF0] write: not connected / no FFF1");
    }
    xSemaphoreGive(_mtx);
    return ok;
}

int BLE_FFF0::writeHex(const String& hex) {
    uint8_t buf[64]; int n = 0;
    String s = hex;
    // parse space/comma-separated hex bytes
    int i = 0, len = s.length();
    while (i < len && n < (int)sizeof(buf)) {
        while (i < len && !isHexadecimalDigit(s.charAt(i))) i++;
        if (i + 1 >= len && i < len) break;   // need two digits
        char tmp[3] = { s.charAt(i), s.charAt(i + 1), 0 };
        if (!isHexadecimalDigit(tmp[1])) { i++; continue; }
        buf[n++] = (uint8_t)strtoul(tmp, nullptr, 16);
        i += 2;
    }
    if (n == 0) { log("[FFF0] writeHex: no bytes parsed"); return -1; }
    return write(buf, n) ? n : -1;
}

// ============================================================================
//  Internal: connect + locate FFF1 + subscribe
// ============================================================================
bool BLE_FFF0::connectAndSubscribe(const String& address, esp_ble_addr_type_t addrType) {
    _lastAddr     = address;
    _lastAddrType = addrType;
    _fff1 = nullptr;

    log("[FFF0] connecting to %s (addrType=%s)...",
        address.c_str(), addrType == BLE_ADDR_TYPE_RANDOM ? "RANDOM" : "PUBLIC");

    _client = BLEDevice::createClient();
    _client->setClientCallbacks(_clientCb);

    BLEAddress bleAddr(std::string(address.c_str()));
    if (!_client->connect(bleAddr, addrType)) {
        log("[FFF0] connect FAILED (check addrType / range / peripheral advertising)");
        delete _client; _client = nullptr; return false;
    }
    log("[FFF0] connected");
    _discFlag = false;
    _client->setMTU(185);

    auto subscribeChar = [&](BLERemoteCharacteristic* c, bool isFff1) -> bool {
        if (!c || (!c->canNotify() && !c->canIndicate())) return false;
        c->registerForNotify(_notifyCb, c->canNotify());   // writes CCCD (0x2902) itself
        char ub[48];
        BLEUUID u = c->getUUID();
        if (u.bitSize() == 16) snprintf(ub, sizeof(ub), "0x%04X", u.getNative()->uuid.uuid16);
        else                   snprintf(ub, sizeof(ub), "128:%s", u.toString().c_str());
        log("[FFF0] subscribed %s (handle %u, %s)%s",
            ub, (unsigned)c->getHandle(),
            c->canNotify() ? "notify" : "indicate", isFff1 ? "  <- FFF1" : "");
        return true;
    };

    auto containsCI = [](const std::string& hay, const char* needle) {
        std::string h = hay; for (auto& ch : h) ch = (char)tolower(ch);
        std::string n = needle; for (auto& ch : n) ch = (char)tolower(ch);
        return h.find(n) != std::string::npos;
    };

    BLERemoteService* svc = _client->getService(UUID_UART_SERVICE);
    if (svc) {
        BLERemoteCharacteristic* c = svc->getCharacteristic(UUID_UART_CHAR);
        if (!c) {   // Fallback A1: FFF1 exposed as a full 128-bit UUID.
            auto* cs = svc->getCharacteristics();
            if (cs) for (auto& kvc : *cs)
                if (containsCI(kvc.second->getUUID().toString(), "fff1")) { c = kvc.second; break; }
            if (c) log("[FFF0] FFF1 matched by 128-bit UUID string");
        }
        if (subscribeChar(c, true)) {
            _fff1 = c;
        } else {
            // Fallback A2: subscribe to the first notifiable char inside FFF0.
            log("[FFF0] FFF1 not found/notifiable; using first notifiable char in FFF0");
            auto* cs = svc->getCharacteristics();
            if (cs) for (auto& kvc : *cs) if (subscribeChar(kvc.second, false)) break;
        }
    } else {
        // Fallback B: no FFF0 service — probe every notifiable char on the device.
        log("[FFF0] No FFF0 service -> PROBE mode (subscribing to ALL notifiable chars)");
        auto* svcs = _client->getServices();
        if (svcs) for (auto& kvs : *svcs) {
            auto* cs = kvs.second->getCharacteristics();
            if (!cs) continue;
            for (auto& kvc : *cs) subscribeChar(kvc.second, false);
        }
    }

    if (_fff1 && _fff1->canRead()) {   // optional initial read
        std::string v = _fff1->readValue();
        if (!v.empty()) {
            log("[FFF0] init read (%u bytes)", (unsigned)v.size());
            if (_dataCb) _dataCb((const uint8_t*)v.data(), v.size());
        }
    }

    if (!_fff1) log("[FFF0] NOTE: FFF1 not identified — data will still arrive via notify");
    log("[FFF0] ready; waiting for data from peripheral");
    return true;
}

// ============================================================================
//  Internal callbacks (called from BLE tasks)
// ============================================================================
void BLE_FFF0::_notifyCb(BLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
    if (len && s_inst && s_inst->_dataCb) s_inst->_dataCb(data, len);
}

void BLE_FFF0::_onScanResult(const String& addr, const String& name,
                             int8_t rssi, esp_ble_addr_type_t addrType) {
    for (auto& d : _devices) {
        if (d.address.equalsIgnoreCase(addr)) {
            if (rssi > d.rssi) d.rssi = rssi;
            d.addrType = addrType;
            return;   // dedup
        }
    }
    _devices.push_back({ addr, name, rssi, addrType });
}

// ============================================================================
//  Monitor task: detect drops, fire onDisconnect, auto-reconnect
// ============================================================================
void BLE_FFF0::monitorTask(void* arg) {
    BLE_FFF0* self = (BLE_FFF0*)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!self->_connected) continue;

        bool alive = self->_client && self->_client->isConnected();
        if (self->_discFlag || !alive) {
            // Tear down. The monitor task owns the client now (onDisconnect ran
            // on the host task and only set the flag, per Bluedroid rules).
            xSemaphoreTake(self->_mtx, portMAX_DELAY);
            self->_connected = false;
            self->_discFlag  = false;
            if (self->_client) { self->_client->disconnect(); delete self->_client; self->_client = nullptr; }
            self->_fff1 = nullptr;
            xSemaphoreGive(self->_mtx);

            if (self->_discCb) self->_discCb();

            if (self->_autoReconnect > 0) {
                bool ok = false;
                for (int i = 0; i < self->_autoReconnect; ++i) {
                    self->log("[FFF0] reconnect %d/%d ...", i + 1, self->_autoReconnect);
                    xSemaphoreTake(self->_mtx, portMAX_DELAY);
                    ok = self->connectAndSubscribe(self->_lastAddr, self->_lastAddrType);
                    self->_connected = ok;
                    xSemaphoreGive(self->_mtx);
                    if (ok) break;
                    vTaskDelay(pdMS_TO_TICKS(800));
                }
                if (!ok) self->log("[FFF0] reconnect failed");
            }
        }
    }
}

// ============================================================================
//  Logging
// ============================================================================
void BLE_FFF0::log(const char* fmt, ...) {
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (_logCb) _logCb(String(buf));
    else        Serial.println(buf);
}
