#include "S3XYButton.h"

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// --- feature detect: USB-CDC present AND enabled? ---
#ifndef S3XY_HAS_USB
#if (defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT) || (defined(USBCON) && USBCON) || \
    (defined(ARDUINO_TINYUSB_ENABLE) && ARDUINO_TINYUSB_ENABLE)
#define S3XY_HAS_USB 1
#else
#define S3XY_HAS_USB 0
#endif
#endif

static inline bool serial_ready() {
#if S3XY_HAS_USB
  return (bool)Serial;  // true when host attached on USB CDC
#else
  return true;  // UART prints anytime
#endif
}
#define S3XY_LOGF(fmt, ...)                                \
  do {                                                     \
    if (serial_ready()) Serial.printf(fmt, ##__VA_ARGS__); \
  } while (0)
#define S3XY_LOG(msg)                        \
  do {                                       \
    if (serial_ready()) Serial.println(msg); \
  } while (0)

// ---- UUIDs ----
static const char* SERVICE_UUID = "00003d46-87d2-479e-7e45-8551415a6de1";
static const char* CHAR_NOTIFY_UUID = "00003d50-87d2-479e-7e45-8551415a6de1";
static const char* CHAR_ID_UUID = "00003d49-87d2-479e-7e45-8551415a6de1";

// ---- State (internal) ----
static BLEServer* g_server = nullptr;
static BLECharacteristic* g_notify = nullptr;
static BLECharacteristic* g_id = nullptr;
static bool g_connected = false;
static bool g_subscribed = false;

// Default ID (edit via s3xy_set_id)
static uint8_t g_buttonId[10] = {'S', 'A', 'B', 'R', 'I', 'N', 'A', 'S', 'N', 'S'};

// User hooks
static s3xy_cb_t g_onConnect = nullptr;
static s3xy_cb_t g_onDisconnect = nullptr;

// ---- Helpers ----
static inline void notifyBytes(const uint8_t* data, size_t len) {
  if (!g_subscribed || !g_notify) return;
  g_notify->setValue((uint8_t*)data, len);
  g_notify->notify();
}

// ---- Callbacks for BLE (internal) ----
class SecCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    return 0;
  }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override {
    return true;
  }
  bool onConfirmPIN(uint32_t) override {
    return true;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    S3XY_LOG(cmpl.success ? "Bonded" : "Bond failed");
  }
};

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    g_connected = true;
    S3XY_LOG("Connected");
    const uint8_t poke = 0x00;
    notifyBytes(&poke, 1);
    if (g_onConnect) g_onConnect();
  }
  void onDisconnect(BLEServer*) override {
    g_connected = false;
    g_subscribed = false;
    S3XY_LOG("Disconnected");
    if (g_onDisconnect) g_onDisconnect();
    BLEDevice::startAdvertising();
  }
};

class CccdCB : public BLEDescriptorCallbacks {
  void onWrite(BLEDescriptor* d) override {
    uint8_t* value = d->getValue();
    size_t len = d->getLength();

    g_subscribed = len >= 1 && (value[0] & 0x01);
    S3XY_LOG(g_subscribed ? "Notifications enabled" : "Notifications disabled");
  }
};

class IDCB : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic*) override {
    S3XY_LOG("ID read");
  }

  void onWrite(BLECharacteristic* c) override {
    uint8_t* data = c->getData();
    size_t len = c->getLength();

    S3XY_LOGF("ID write (%u): ", (unsigned)len);
    for (size_t i = 0; i < len; ++i) Serial.printf("%02X ", data[i]);
    Serial.println();

    if (len == 1 && data[0] == 0xB6) {  // Set mode ?
      const uint8_t resp[] = {0xC7, 0x00, 0x01};
      notifyBytes(resp, sizeof(resp));
      S3XY_LOG("Commander initialized success");
    } else if (len == 1 && data[0] == 0xA1) {  // Disconnect request
      S3XY_LOG("Unpair -> disconnect");
      g_server->disconnect(0);
    } else if (len == 4 && data[0] == 0xA4) {  // Rename request
      const uint8_t resp[] = {0xA4, 0x00, data[1], data[2]};
      notifyBytes(resp, sizeof(resp));
      S3XY_LOG("Rename handled");
    }
  }
};

// ---- Public API ----
void s3xy_begin(const char* deviceName) {
  Serial.begin(115200);
#if S3XY_HAS_USB
  uint32_t end = millis() + 1000;
  while (!Serial && millis() < end) delay(10);
#endif

  BLEDevice::init(deviceName ? deviceName : "ENH_BTN");

  static SecCB sec;
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(&sec);

  auto* pSec = new BLESecurity();
  pSec->setCapability(ESP_IO_CAP_NONE);
  pSec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  pSec->setKeySize(16);
  pSec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  BLEService* svc = g_server->createService(SERVICE_UUID);

  g_notify = svc->createCharacteristic(CHAR_NOTIFY_UUID,
                                       BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  auto* cccd = new BLE2902();
  cccd->setNotifications(false);
  cccd->setCallbacks(new CccdCB());
  g_notify->addDescriptor(cccd);
  g_notify->setValue((uint8_t*)"\x00", 1);

  g_id = svc->createCharacteristic(CHAR_ID_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  g_id->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  g_id->setValue(g_buttonId, sizeof(g_buttonId));
  g_id->setCallbacks(new IDCB());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  BLEAdvertisementData advData, scanResp;
  advData.setAppearance(0x0000);
  advData.setFlags(0x06);
  advData.setName(deviceName ? deviceName : "ENH_BTN");
  scanResp.setCompleteServices(BLEUUID(SERVICE_UUID));
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanResp);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  S3XY_LOG("Advertising as spoofed S3XY button");
}

void s3xy_loop() {
  // no periodic work needed
}

bool s3xy_ready() {
  return g_connected && g_subscribed;
}

void s3xy_send_single() {
  if (!s3xy_ready()) return;
  const uint8_t p1[] = {0x01};
  const uint8_t p0[] = {0x00};
  const uint8_t pC1[] = {0xC1, 0x01};
  notifyBytes(p1, sizeof(p1));
  delay(5);
  notifyBytes(p0, sizeof(p0));
  delay(5);
  notifyBytes(pC1, sizeof(pC1));
  delay(5);
  notifyBytes(p0, sizeof(p0));
  delay(5);
}

void s3xy_send_long() {
  if (!s3xy_ready()) return;
  const uint8_t p[] = {0xC3, 0x01};
  notifyBytes(p, sizeof(p));
  delay(5);
}

void s3xy_send_double() {
  if (!s3xy_ready()) return;
  const uint8_t p[] = {0xC1, 0x02};
  notifyBytes(p, sizeof(p));
  delay(5);
}

void s3xy_set_id(const uint8_t id[10]) {
  memcpy(g_buttonId, id, 10);
  if (g_id) g_id->setValue(g_buttonId, sizeof(g_buttonId));
}

// --- Hook setters ---
void s3xy_on_connect(s3xy_cb_t cb) {
  g_onConnect = cb;
}
void s3xy_on_disconnect(s3xy_cb_t cb) {
  g_onDisconnect = cb;
}
