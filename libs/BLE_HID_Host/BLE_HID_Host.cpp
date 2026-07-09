/*
 * BLE_HID_Host — implementation. See BLE_HID_Host.h for the public surface.
 *
 * Built on the same Bluedroid BLEClient API as BLE_FFF0, but targets standard
 * BLE HID gamepads (service 0x1812). The differences from FFF0:
 *   - Just-works bonding (ESP_IO_CAP_NONE + SC_BOND) is configured so the
 *     encrypted HID link comes up; bonds persist to NVS (reconnect w/o re-pair).
 *   - On connect we wait for authentication to complete (a semaphore given by
 *     onAuthenticationComplete) BEFORE subscribing — registerForNotify writes
 *     the CCCD itself but does not trigger pairing, so subscribing too early
 *     fails with ESP_GATT_INSUFF_AUTHENTICATION and no reports ever arrive.
 *   - We read the HID Report Map (0x2A4B), parse it to learn the gamepad
 *     report layout (button bits + D-pad hat), then subscribe to the matching
 *     Report characteristic (0x2A4D, selected via its 0x2908 Report Reference).
 *   - Incoming reports are decoded to {buttons, hat} and pushed via onReport.
 */
#include "BLE_HID_Host.h"

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteDescriptor.h>
#include <BLESecurity.h>

// HID service / characteristics / descriptors.
static const BLEUUID UUID_HID_SERVICE  = BLEUUID((uint16_t)0x1812);
static const BLEUUID UUID_REPORT_MAP   = BLEUUID((uint16_t)0x2A4B);  // Report Map (descriptor)
static const BLEUUID UUID_REPORT       = BLEUUID((uint16_t)0x2A4D);  // Report (notifiable)
static const BLEUUID UUID_RPT_REF      = BLEUUID((uint16_t)0x2908);  // Report Reference desc (id+type)
static const BLEUUID UUID_PROTO_MODE   = BLEUUID((uint16_t)0x2A4E);

// Set to 1 to dump every incoming report's raw bytes (edge-triggered). This is
// the single most useful diagnostic when a pad "connects but no buttons work":
// it shows whether ANY notification arrives and which report ID it carries.
// Turn off (0) once the pad is mapped. Default ON while tuning.
#ifndef K10_BLE_HID_DEBUG
#define K10_BLE_HID_DEBUG 1
#endif

// Single-instance pointer used to route the no-user-data notify + auth callbacks.
BLE_HID_Host* BLE_HID_Host::s_inst = nullptr;

// ============================================================================
//  Scan callbacks
// ============================================================================
class _HIDScanCb : public BLEAdvertisedDeviceCallbacks {
    BLE_HID_Host* _o;
public:
    explicit _HIDScanCb(BLE_HID_Host* o) : _o(o) {}

    void onResult(BLEAdvertisedDevice d) override {
        std::string name = d.getName();
        std::string addr = d.getAddress().toString();
        esp_ble_addr_type_t at = d.getAddressType();

        _o->log("[hid scan] '%s' %s rssi=%d addrType=%s",
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
class _HIDClientCb : public BLEClientCallbacks {
    BLE_HID_Host* _o;
public:
    explicit _HIDClientCb(BLE_HID_Host* o) : _o(o) {}
    void onConnect(BLEClient*) override {}
    void onDisconnect(BLEClient*) override {
        // Runs on the Bluedroid host task — never touch _client here. Just flag.
        _o->_discFlag = true;
        _o->log("[hid] disconnected");
    }
};

// ============================================================================
//  Security callbacks (just-works bonding). All five are pure-virtual.
// ============================================================================
class _HIDSecCb : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override             { return 0; }
    void     onPassKeyNotify(uint32_t pass_key) override { (void)pass_key; }
    bool     onSecurityRequest() override            { return true; }
    bool     onConfirmPIN(uint32_t pin) override     { (void)pin; return true; }
    void     onAuthenticationComplete(esp_ble_auth_cmpl_t ac) override {
        // Runs on the Bluedroid host task. Record outcome and release the connect
        // flow that is blocked waiting on _authSem.
        if (BLE_HID_Host::s_inst) {
            BLE_HID_Host::s_inst->log("[hid] auth %s (addrType=%d auth_mode=0x%02x fail=%d)",
                ac.success ? "OK" : "FAIL",
                (int)ac.addr_type, (int)ac.auth_mode, (int)ac.fail_reason);
            BLE_HID_Host::s_inst->_authDone(ac.success);
        }
    }
};

// ============================================================================
//  Public API
// ============================================================================
void BLE_HID_Host::begin(const char* deviceName) {
    if (!_mtx)     _mtx     = xSemaphoreCreateMutex();
    if (!_authSem) _authSem = xSemaphoreCreateBinary();
    s_inst = this;
    if (!_scanCb)   _scanCb   = new _HIDScanCb(this);
    if (!_clientCb) _clientCb = new _HIDClientCb(this);
    if (!_secCb)    _secCb    = new _HIDSecCb();

    // (1) Static BLE-device-level setup (order-independent, conventionally first).
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_NO_MITM);  // level 2, not MITM
    BLEDevice::setSecurityCallbacks(_secCb);
    // (2) Bring up Bluedroid (idempotent) + request a big MTU so the Report Map
    //     (100-300 bytes) fits in a single GATT read (default MTU 23 truncates it).
    BLEDevice::init(deviceName ? deviceName : "BLE-HID");
    BLEDevice::setMTU(247);
    // (3) SMP params — these call esp_ble_gap_set_security_param, so they MUST
    //     run after init(). Just-works: no IO capability, bond + secure connections.
    BLESecurity sec;
    sec.setCapability(ESP_IO_CAP_NONE);
    sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec.setKeySize(16);

    int nbond = esp_ble_get_bond_device_num();
    log("[hid] ready (bonded devices in NVS: %d)", nbond);

    static bool taskStarted = false;
    if (!taskStarted) {
        taskStarted = true;
        xTaskCreate([](void* arg) {                    // monitor task
            BLE_HID_Host* self = (BLE_HID_Host*)arg;
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if (!self->_connected) continue;
                bool alive = self->_client && self->_client->isConnected();
                if (self->_discFlag || !alive) {
                    xSemaphoreTake(self->_mtx, portMAX_DELAY);
                    self->_connected = false;
                    self->_discFlag  = false;
                    if (self->_client) { self->_client->disconnect(); delete self->_client; self->_client = nullptr; }
                    xSemaphoreGive(self->_mtx);

                    if (self->_discCb) self->_discCb();

                    if (self->_autoReconnect > 0) {
                        bool ok = false;
                        for (int i = 0; i < self->_autoReconnect; ++i) {
                            self->log("[hid] reconnect %d/%d ...", i + 1, self->_autoReconnect);
                            xSemaphoreTake(self->_mtx, portMAX_DELAY);
                            ok = self->connectAndSubscribe(self->_lastAddr, self->_lastAddrType);
                            self->_connected = ok;
                            xSemaphoreGive(self->_mtx);
                            if (ok) break;
                            vTaskDelay(pdMS_TO_TICKS(800));
                        }
                        if (!ok) self->log("[hid] reconnect failed");
                    }
                }
            }
        }, "hid_mon", 4096, this, 1, nullptr);
    }
}

void BLE_HID_Host::scan(uint32_t durationMs) {
    _devices.clear();
    BLEScan* s = BLEDevice::getScan();
    s->setAdvertisedDeviceCallbacks(_scanCb, false);
    s->setActiveScan(true);
    s->setInterval(80);
    s->setWindow(60);
    uint32_t secs = durationMs / 1000; if (secs == 0) secs = 1;
    log("[hid] scanning %us...", (unsigned)secs);
    s->start(secs);   // blocking
    log("[hid] scan done: %d device(s)", (int)_devices.size());
}

void BLE_HID_Host::clearDevices() { _devices.clear(); }
int  BLE_HID_Host::deviceCount() const { return (int)_devices.size(); }

const BLE_HID_Host::Device* BLE_HID_Host::device(int index) const {
    if (index < 0 || index >= (int)_devices.size()) return nullptr;
    return &_devices[(size_t)index];
}

bool BLE_HID_Host::connect(int deviceIndex) {
    const Device* d = device(deviceIndex);
    if (!d) { log("[hid] connect: bad index %d", deviceIndex); return false; }
    return connect(d->address, d->addrType);
}

bool BLE_HID_Host::connect(const String& address, esp_ble_addr_type_t addrType) {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    bool ok = connectAndSubscribe(address, addrType);
    _connected = ok;
    xSemaphoreGive(_mtx);
    return ok;
}

void BLE_HID_Host::disconnect() {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    _connected = false;
    _discFlag  = false;
    if (_client) { _client->disconnect(); delete _client; _client = nullptr; }
    xSemaphoreGive(_mtx);
    log("[hid] disconnected on request");
}

bool BLE_HID_Host::isConnected() const { return _connected; }

// Called from the host task by onAuthenticationComplete: stash the outcome and
// unblock connectAndSubscribe()'s wait on _authSem.
void BLE_HID_Host::_authDone(bool ok) {
    // _authOk lives in the class; store via a small member. We reuse _discFlag's
    // pattern: a volatile bool the connect flow reads right after the sem fires.
    _authOk = ok;
    xSemaphoreGive(_authSem);
}

// ============================================================================
//  Internal: connect + bond + discover HID service + subscribe
// ============================================================================
bool BLE_HID_Host::connectAndSubscribe(const String& address, esp_ble_addr_type_t addrType) {
    _lastAddr     = address;
    _lastAddrType = addrType;

    log("[hid] connecting to %s (addrType=%s)...",
        address.c_str(), addrType == BLE_ADDR_TYPE_RANDOM ? "RANDOM" : "PUBLIC");

    if (_client) { _client->disconnect(); delete _client; _client = nullptr; }
    _client = BLEDevice::createClient();
    _client->setClientCallbacks(_clientCb);

    BLEAddress bleAddr(std::string(address.c_str()));

    // Drain any stale auth signal, then connect. BLEClient::connect fires
    // CONNECT_EVT, where the library auto-calls esp_ble_set_encryption (because
    // we set an encryption level in begin()) -> SMP pairing -> onAuthenticationComplete.
    xSemaphoreTake(_authSem, 0);
    _authOk = false;
    if (!_client->connect(bleAddr, addrType)) {
        log("[hid] connect FAILED (check addrType / range / pad advertising)");
        delete _client; _client = nullptr; return false;
    }
    _discFlag = false;
    _client->setMTU(247);

    // Wait for pairing/bonding to finish before touching GATT (see file header).
    if (xSemaphoreTake(_authSem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        log("[hid] auth timeout (5s) — pad may need a different IO capability");
        _client->disconnect(); delete _client; _client = nullptr; return false;
    }
    if (!_authOk) {
        log("[hid] auth failed — pad likely requires MITM (numeric comparison / passkey)");
        _client->disconnect(); delete _client; _client = nullptr; return false;
    }
    // Give the MTU exchange a moment to settle so the Report Map read isn't short.
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!discoverAndSubscribe()) {
        _client->disconnect(); delete _client; _client = nullptr; return false;
    }
    log("[hid] ready; waiting for reports");
    return true;
}

bool BLE_HID_Host::discoverAndSubscribe() {
    BLERemoteService* svc = _client->getService(UUID_HID_SERVICE);
    if (!svc) {
        // Encryption sometimes finishes a hair after connect completes; retry once.
        vTaskDelay(pdMS_TO_TICKS(200));
        svc = _client->getService(UUID_HID_SERVICE);
    }
    if (!svc) {
        log("[hid] no HID service (0x1812) — not an HID device?");
        return false;
    }

    // ---- Read + parse the Report Map -------------------------------------
    _layout = HidLayout{};            // reset to defaults (valid=false)
    BLERemoteCharacteristic* rm = svc->getCharacteristic(UUID_REPORT_MAP);
    if (rm && rm->canRead()) {
        std::string v = rm->readValue();
        log("[hid] report map: %u bytes", (unsigned)v.size());
        if (v.size() >= 2) {
            parseReportMap((const uint8_t*)v.data(), v.size());
        }
    } else {
        log("[hid] report map char not readable");
    }
    if (_layout.valid) {
        log("[hid] layout: reportId=%d hasReportId=%d btn@%u cnt=%u hat@%u/%u len=%u",
            (int)_layout.reportId, _layout.hasReportId ? 1 : 0,
            (unsigned)_layout.btnBitOffset, (unsigned)_layout.btnBitCount,
            (unsigned)_layout.hatBitOffset, (unsigned)_layout.hatBits,
            (unsigned)_layout.reportLen);
    } else {
        log("[hid] auto-detect failed -> fixed fallback (no rptID, btn=byte0, hat=byte1&0x0F)");
    }

    // ---- Subscribe to ALL notifiable Report characteristics --------------
    // A pad can expose one Report char per report ID, OR a single char that
    // multiplexes several report IDs (declaring only one Report Reference, or a
    // wrong one). Filtering down to "the char whose rid matches the parsed
    // report ID" is fragile — cheap pads mis-declare the Report Reference, and
    // you end up subscribed to the consumer-control char while the gamepad reports
    // (a different report ID) never arrive (this is exactly the "connected but no
    // button log" symptom). Subscribing to every notifiable/indicatable Report
    // char is safe: reports whose ID isn't the gamepad's are dropped in
    // decodeReport(), so extra subscriptions cost nothing and can't be wrong.
    // IMPORTANT: use getCharacteristicsByHandle(), NOT getCharacteristics(). The
    // latter returns a std::map keyed by UUID *string*, and std::map::insert is
    // silently a no-op on a duplicate key — so two Report chars that share UUID
    // 0x2A4D (one per report ID, the standard HID-over-GATT layout) collapse into
    // a single map entry and the second is dropped. That is exactly how a pad ends
    // up "connected but no button log": only the first Report char (e.g. consumer-
    // control rid=3) is visible, the gamepad Report char (rid=4) is hidden, its
    // reports never arrive. The by-handle map keys on the unique attribute handle,
    // so every characteristic survives.
    auto* chars = svc->getCharacteristicsByHandle();
    if (!chars) { log("[hid] no characteristics under 0x1812"); return false; }

    int reportChars = 0, subscribed = 0;
    for (auto& kv : *chars) {
        BLERemoteCharacteristic* c = kv.second;
        if (!c) continue;
        if (!c->getUUID().equals(UUID_REPORT)) continue;     // only Report (0x2A4D) chars
        reportChars++;

        bool canN = c->canNotify(), canI = c->canIndicate();

        // Resolve this Report's id+type via its 0x2908 Report Reference descriptor
        // (informational — we no longer gate subscription on it).
        uint8_t rid = 0, rtype = 0;
        bool hasRef = false;
        auto* descs = c->getDescriptors();
        if (descs) for (auto& dkv : *descs) {
            if (dkv.second && dkv.second->getUUID().equals(UUID_RPT_REF)) {
                std::string rv = dkv.second->readValue();
                if (rv.size() >= 2) { rid = (uint8_t)rv[0]; rtype = (uint8_t)rv[1]; hasRef = true; }
                break;
            }
        }

        bool want = canN || canI;   // subscribe to every notifiable Report char
        log("[hid] report char handle=%u notify=%d indicate=%d ref=%d rid=%d type=%d -> %s",
            (unsigned)c->getHandle(), canN ? 1 : 0, canI ? 1 : 0,
            hasRef ? 1 : 0, rid, rtype, want ? "SUBSCRIBE" : "skip");
        if (!want) continue;

        c->registerForNotify(_notifyCb, canN);   // writes CCCD (0x2902) itself
        ++subscribed;
    }
    log("[hid] found %d Report char(s), subscribed to %d", reportChars, subscribed);

    // Optionally ask the pad for Report Mode (0x00) over Boot Mode.
    BLERemoteCharacteristic* pm = svc->getCharacteristic(UUID_PROTO_MODE);
    if (pm && (pm->canWrite() || pm->canWriteNoResponse())) {
        uint8_t zero = 0x00;
        pm->writeValue(&zero, 1, pm->canWrite());
    }

    if (subscribed == 0) {
        log("[hid] subscribed to 0 report chars — no notifiable Report char");
        return false;
    }
    return true;
}

// ============================================================================
//  HID Report Map descriptor parser (minimal; see HIDTypes.h for item tags)
// ============================================================================
void BLE_HID_Host::parseReportMap(const uint8_t* d, size_t len) {
    // Global state (with a 1-deep push/pop stack — enough for gamepads).
    uint16_t usagePage = 0, reportSize = 0, reportCount = 0;
    int32_t  logMin = 0, logMax = 0;
    uint16_t curReportId = 0;
    bool     anyReportId = false;
    // Local state (per Input item).
    uint16_t usage = 0;

    // Bit offset within the current report's Input fields (excludes the report
    // ID byte — HID counts bits from the first data byte after the report ID).
    uint16_t bitOff = 0;
    // No collection scoping: we capture the first Button field (Usage Page 0x09)
    // and the first D-pad hat (Usage 0x39) found in ANY report. The Usage Page is
    // the right discriminator — gamepad buttons are always 0x09, media/keyboard
    // keys are 0x0C/0x07 — and crucially it survives pads that put Consumer
    // Control in the FIRST Application collection and the Game Pad in a second
    // one (this pad does exactly that), which defeated the old "first app
    // collection only" heuristic and left _layout invalid (fallback mis-decode).

    // One-level global stack for PUSH/POP.
    struct G { uint16_t usagePage, reportSize, reportCount; int32_t logMin, logMax; };
    G stack[4]; int sp = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = d[i];
        if (b == 0xFE) {                // long item — skip
            if (i + 1 >= len) break;
            i += 2 + d[i + 1] + 1;
            continue;
        }
        uint8_t bSize = b & 0x03; if (bSize == 3) bSize = 4;   // size 3 -> 4 bytes
        uint8_t bType = (b >> 2) & 0x03;
        uint8_t bTag  = (b >> 4) & 0x0F;
        i += 1;
        if (i + bSize > len) break;
        int32_t val = 0;
        for (uint8_t k = 0; k < bSize; k++) val |= (int32_t)d[i + k] << (8 * k);
        if (bSize == 4) { /* keep as-is */ }
        else if (bSize == 1 && (d[i] & 0x80)) val |= (int32_t)0xFFFFFF00;   // sign-extend
        else if (bSize == 2 && (d[i + 1] & 0x80)) val |= (int32_t)0xFFFF0000;
        i += bSize;

        if (bType == 1) {               // Global
            switch (bTag) {
                case 0x0: usagePage  = (uint16_t)val; break;        // Usage Page
                case 0x1: logMin     = val; break;                  // Logical Minimum
                case 0x2: logMax     = val; break;                  // Logical Maximum
                case 0x7: reportSize = (uint16_t)val; break;        // Report Size
                case 0x8:            {                              // Report ID
                    if (!anyReportId) { anyReportId = true; bitOff = 0; }
                    else if ((uint16_t)val != curReportId) { curReportId = (uint16_t)val; bitOff = 0; }
                    curReportId = (uint16_t)val;
                    break;
                }
                case 0x9: reportCount = (uint16_t)val; break;       // Report Count
                case 0xA: if (sp < 4) { stack[sp++] = {usagePage, reportSize, reportCount, logMin, logMax}; } break;  // Push
                case 0xB: if (sp > 0) { G g = stack[--sp]; usagePage=g.usagePage; reportSize=g.reportSize; reportCount=g.reportCount; logMin=g.logMin; logMax=g.logMax; } break;  // Pop
            }
        } else if (bType == 2) {        // Local (reset implicitly each Input)
            switch (bTag) {
                case 0x0: usage = (uint16_t)val; break;             // Usage
            }
        } else if (bType == 0) {        // Main
            switch (bTag) {
                case 0xA:               // Collection
                    usage = 0;          // local usage resets per main item
                    break;
                case 0xC:               // End Collection (nothing to track now)
                    break;
                case 0x8: {             // Input
                    if (usagePage == 0x09 && reportSize == 1 && !_layout.valid) {
                        // Button field. Bit offset is absolute within the report
                        // data (after the report ID byte) — recorded as-is; the
                        // report ID byte is stripped in decodeReport.
                        _layout.valid        = true;
                        _layout.hasReportId  = anyReportId;
                        _layout.reportId     = (uint8_t)curReportId;
                        _layout.btnBitOffset = bitOff;
                        _layout.btnBitCount  = (uint8_t)reportCount;
                    } else if (usagePage == 0x01 && usage == 0x39 && !_layout.hasHat) {
                        // D-pad hat switch (4-bit, logical 0..7).
                        _layout.hasHat       = true;
                        _layout.hatBitOffset = bitOff;
                        _layout.hatBits      = (uint8_t)reportSize;
                    }
                    bitOff += reportCount * reportSize;
                    usage = 0;           // local usage resets per main item
                    break;
                }
                // Output(0x9) / Feature(0xB): don't advance the Input bit offset.
            }
        }
    }

    if (_layout.valid) {
        _layout.reportLen = (uint16_t)((bitOff + 7) / 8 + (anyReportId ? 1 : 0));
    }
}

// Extract `nBits` bits starting at bit `bitOff` (little-endian within bytes, per HID).
static uint32_t extractBits(const uint8_t* d, size_t len, uint16_t bitOff, uint8_t nBits) {
    if (nBits == 0 || nBits > 32) return 0;
    uint32_t v = 0;
    for (uint8_t k = 0; k < nBits; k++) {
        uint32_t bp = (uint32_t)bitOff + k;
        if (bp / 8 >= len) break;
        if ((d[bp / 8] >> (bp % 8)) & 1) v |= (1UL << k);
    }
    return v;
}

void BLE_HID_Host::decodeReport(const uint8_t* data, size_t len) {
    if (!data || !len) return;

#if K10_BLE_HID_DEBUG
    // Edge-triggered dump of EVERY incoming notification (pads spam reports while
    // idle, so we only log when the raw bytes change). Deliberately BEFORE the
    // report-ID filter below: non-gamepad reports (e.g. consumer-control report
    // ID 3) are dropped silently there, and this is the only way to tell "no
    // reports at all" (CCCD / subscription problem) from "reports arrive but on
    // the wrong report ID" (routing problem). When hasReportId, data[0] is the
    // report ID — for this pad it should be 04 for button/stick reports.
    {
        static uint8_t last[16] = {0};
        uint8_t n = len < 16 ? (uint8_t)len : 16;
        bool changed = false;
        for (uint8_t k = 0; k < n; k++) if (data[k] != last[k]) { changed = true; break; }
        if (changed) {
            char hex[64] = {0};
            for (uint8_t k = 0; k < n; k++) snprintf(hex + k * 3, 4, "%02X ", data[k]);
            log("[hid] rpt len=%u %s", (unsigned)len, hex);
            memcpy(last, data, n);
        }
    }
#endif

    GamepadState st;

    const uint8_t* p = data;
    size_t plen = len;
    if (_layout.valid) {
        if (_layout.hasReportId) {
            // The Report Map declares a Report ID, so the layout's bit offsets are
            // measured from the byte AFTER the ID byte. But whether that ID byte is
            // actually in the payload varies: conformant pads send it, while some
            // (this one) declare a Report ID yet emit reports WITHOUT the leading ID
            // byte — a 10-byte gamepad report instead of the declared 11, which made
            // data[0] (an axis value 0x80) fail the old "data[0] == reportId" test so
            // every report was dropped and no button ever decoded. Decide by length:
            //   len == reportLen     -> ID byte present, strip it
            //   len == reportLen - 1 -> ID byte absent, use payload as-is
            //   else                 -> trust data[0] == reportId
            // The bit offsets are relative to the first data byte in all cases, so
            // they need no adjustment either way.
            bool stripId = false;
            if (len >= 1 && (uint16_t)len == _layout.reportLen) {
                if (data[0] != _layout.reportId) return;   // a different report (e.g. media keys)
                stripId = true;
            } else if (len >= 1 && (uint16_t)len + 1 == _layout.reportLen) {
                stripId = false;                            // pad omits the ID byte
            } else {
                stripId = (data[0] == _layout.reportId);
            }
            if (stripId) { p = data + 1; plen = len - 1; }
        }
        st.buttons = extractBits(p, plen, _layout.btnBitOffset, _layout.btnBitCount);
        if (_layout.hasHat) {
            uint32_t h = extractBits(p, plen, _layout.hatBitOffset, _layout.hatBits);
            st.hat = (h <= 7) ? (uint8_t)h : 0xFF;
        } else {
            st.hat = 0xFF;
        }
    } else {
        // Fixed fallback: [buttons8][hat4 | 4 spare], no report ID.
        st.buttons = (len >= 1) ? data[0] : 0;
        st.hat     = (len >= 2) ? (uint8_t)(data[1] & 0x0F) : 0xFF;
        if (st.hat > 7) st.hat = 0xFF;
    }

    if (_reportCb) _reportCb(st);
}

void BLE_HID_Host::_notifyCb(BLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
    if (len && s_inst) s_inst->decodeReport(data, len);
}

void BLE_HID_Host::_onScanResult(const String& addr, const String& name,
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
//  Logging
// ============================================================================
void BLE_HID_Host::log(const char* fmt, ...) {
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (_logCb) _logCb(String(buf));
    else        Serial.println(buf);
}
