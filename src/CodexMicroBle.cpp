// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#include "CodexMicroBle.h"

#include <BLE2902.h>
#include <BLEDescriptor.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEUtils.h>

#include "BoardProfile.h"

#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#endif

namespace {

constexpr char kDeviceName[] = "Codex Micro";
constexpr char kManufacturer[] = "Work Louder";
constexpr size_t kPayloadSize = 61;
constexpr size_t kReportBodySize = 63;

constexpr uint16_t swapBytes(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

// One vendor-defined input/output report. HIDAPI adds/removes Report ID 6,
// while the BLE characteristics carry the remaining 63-byte report body.
const uint8_t kReportMap[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (1)
    0xA1, 0x01,              // Collection (Application)
    0x85, 0x06,              // Report ID (6)
    0x15, 0x00,              // Logical Minimum (0)
    0x26, 0xFF, 0x00,        // Logical Maximum (255)
    0x75, 0x08,              // Report Size (8)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x01,              // Usage (1)
    0x81, 0x02,              // Input (Data, Variable, Absolute)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x02,              // Usage (2)
    0x91, 0x02,              // Output (Data, Variable, Absolute)
    0xC0                     // End Collection
};

}  // namespace

class CodexSecurityCallbacks final : public BLESecurityCallbacks {
 public:
  explicit CodexSecurityCallbacks(CodexMicroBle& owner) : owner_(owner) {}
  bool onSecurityRequest() override { return true; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  #if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    Serial.printf("BLE pairing %s\n", result.success ? "complete" : "failed");
    owner_.onSecurity(result.success, result.success, result.success, result.success);
  }
  #elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* result) override {
    const bool encrypted = result != nullptr && result->sec_state.encrypted;
    const bool authenticated = result != nullptr && result->sec_state.authenticated;
    const bool bonded = result != nullptr && result->sec_state.bonded;
    const bool authorized = result != nullptr && result->sec_state.authorize;
    Serial.printf("BLE security encrypted=%u authenticated=%u bonded=%u authorized=%u\n",
                  encrypted, authenticated, bonded, authorized);
    owner_.onSecurity(encrypted, authenticated, bonded, authorized);
  }
  #endif

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onConnect(BLEServer*) override { owner_.onConnected(true); }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer*, ble_gap_conn_desc* desc) override {
    if (desc == nullptr) return;
    Serial.printf("BLE GAP handle=%u interval=%u latency=%u timeout=%u\n",
                  desc->conn_handle, desc->conn_itvl, desc->conn_latency,
                  desc->supervision_timeout);
    int rc = 0;
    const bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
    Serial.printf("BLE security start=%u rc=%d\n", started, rc);
  }

  void onDisconnect(BLEServer*, ble_gap_conn_desc* desc) override {
    if (desc != nullptr) {
      Serial.printf("BLE GAP disconnected handle=%u encrypted=%u bonded=%u\n",
                    desc->conn_handle, desc->sec_state.encrypted,
                    desc->sec_state.bonded);
    }
  }

  void onMtuChanged(BLEServer*, ble_gap_conn_desc* desc, uint16_t mtu) override {
    Serial.printf("BLE MTU handle=%u mtu=%u\n",
                  desc == nullptr ? 0xffff : desc->conn_handle, mtu);
  }
#endif

  void onDisconnect(BLEServer*) override {
    owner_.onConnected(false);
    if (!owner_.clearingBonds_.load()) {
      BLEDevice::startAdvertising();
      Serial.println("BLE advertising restarted after disconnect");
    }
  }

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::InputCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit InputCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onStatus(BLECharacteristic*, Status status, uint32_t code) override {
    owner_.onNotifyStatus(static_cast<int>(status), code);
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc* desc,
                   uint16_t value) override {
    Serial.printf("BLE input subscription handle=%u value=%u\n",
                  desc == nullptr ? 0xffff : desc->conn_handle, value);
    owner_.onSubscribed(value);
  }
#endif

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::OutputCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit OutputCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic) override {
    const auto value = characteristic->getValue();
    Serial.printf("BLE output write bytes=%u\n", static_cast<unsigned>(value.length()));
    owner_.onOutput(reinterpret_cast<const uint8_t*>(value.c_str()), value.length());
  }

 private:
  CodexMicroBle& owner_;
};

void CodexMicroBle::begin() {
  stateMutex_ = xSemaphoreCreateMutex();

  BLEDevice::init(kDeviceName);
  BLEDevice::setSecurityCallbacks(new CodexSecurityCallbacks(*this));

  auto* security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(ESP_LE_AUTH_BOND);
#if defined(CONFIG_NIMBLE_ENABLED)
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setForceAuthentication(true);
#endif

  server_ = BLEDevice::createServer();
  server_->setCallbacks(new ServerCallbacks(*this));

  hid_ = new BLEHIDDevice(server_);
  hid_->manufacturer()->setValue(kManufacturer);
  // Low release bits mark the transport as wireless in the desktop bridge.
  // Both selected Arduino BLE implementations serialize these fields
  // big-endian, while the PnP characteristic is defined as little-endian.
  hid_->pnp(0x02, swapBytes(kVendorId), swapBytes(kProductId), swapBytes(0x0101));
  hid_->hidInfo(0x00, 0x01);
  hid_->reportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

#if defined(CONFIG_NIMBLE_ENABLED)
  // Build the Tab5 report characteristics directly.  This lets us establish
  // their concrete value lengths before the HID service is registered with
  // NimBLE (and, through the C6 controller, exposed to Windows).
  BLEService* hidService = hid_->hidService();
  input_ = hidService->createCharacteristic(
      BLEUUID(static_cast<uint16_t>(0x2A4D)),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_READ_ENC |
          BLECharacteristic::PROPERTY_NOTIFY);
  output_ = hidService->createCharacteristic(
      BLEUUID(static_cast<uint16_t>(0x2A4D)),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR |
          BLECharacteristic::PROPERTY_READ_ENC |
          BLECharacteristic::PROPERTY_WRITE_ENC);

  auto* inputReference =
      new BLEDescriptor(BLEUUID(static_cast<uint16_t>(0x2908)), 2);
  const uint8_t inputReferenceValue[] = {kReportId, 0x01};
  inputReference->setValue(inputReferenceValue, sizeof(inputReferenceValue));
  inputReference->setAccessPermissions(ESP_GATT_PERM_READ);
  input_->addDescriptor(inputReference);

  auto* outputReference =
      new BLEDescriptor(BLEUUID(static_cast<uint16_t>(0x2908)), 2);
  const uint8_t outputReferenceValue[] = {kReportId, 0x02};
  outputReference->setValue(outputReferenceValue, sizeof(outputReferenceValue));
  outputReference->setAccessPermissions(ESP_GATT_PERM_READ);
  output_->addDescriptor(outputReference);

  // Windows' HID-over-GATT bridge validates WriteFile buffers against the
  // registered Report characteristic value length. Seed both values before
  // service registration so the GATT database and Windows HID collection
  // agree from the first enumeration.
  uint8_t emptyReport[kReportBodySize] = {};
  input_->setValue(emptyReport, sizeof(emptyReport));
  output_->setValue(emptyReport, sizeof(emptyReport));
#else
  input_ = hid_->inputReport(kReportId);
  output_ = hid_->outputReport(kReportId);
#endif
  input_->setCallbacks(new InputCallbacks(*this));
  output_->setCallbacks(new OutputCallbacks(*this));
  hid_->startServices();
  Serial.printf("BLE reports input_handle=%u output_handle=%u input_len=%u output_len=%u\n",
                input_->getHandle(), output_->getHandle(),
                static_cast<unsigned>(input_->getLength()),
                static_cast<unsigned>(output_->getLength()));
  hid_->setBatteryLevel(batteryPercentage_);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(GENERIC_HID);
  advertising->addServiceUUID(hid_->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf(
      "BLE vendor HID ready VID=%04X PID=%04X usage=FF00 report=%u\n",
      kVendorId, kProductId, kReportId);
}

void CodexMicroBle::setBattery(uint8_t percentage, bool charging) {
  batteryPercentage_ = constrain(percentage, 0, 100);
  charging_ = charging;
  if (hid_ != nullptr && connected()) {
    hid_->setBatteryLevel(batteryPercentage_);
  }
}

void CodexMicroBle::sendKey(const char* key, uint8_t action, int8_t agent) {
  StaticJsonDocument<192> message;
  message["method"] = "v.oai.hid";
  JsonObject params = message.createNestedObject("params");
  params["k"] = key;
  params["act"] = action;
  if (agent >= 0) {
    params["ag"] = agent;
  }

  String json;
  serializeJson(message, json);
  sendJson(json);
  Serial.printf("HID key=%s action=%u\n", key, action);
}

void CodexMicroBle::sendJoystick(float angle, float distance) {
  StaticJsonDocument<160> message;
  message["method"] = "v.oai.rad";
  JsonObject params = message.createNestedObject("params");
  params["a"] = angle;
  params["d"] = distance;

  String json;
  serializeJson(message, json);
  sendJson(json);
}

bool CodexMicroBle::clearBonds() {
  clearingBonds_ = true;
  BLEDevice::getAdvertising()->stop();
  Serial.println("BLE advertising stopped for unpair");
  bool success = true;

  if (server_ != nullptr && connected()) {
    Serial.println("Disconnecting BLE host for unpair");
    server_->disconnect(server_->getConnId());
    const uint32_t deadline = millis() + 2000;
    while (connected() && static_cast<int32_t>(deadline - millis()) > 0) {
      delay(10);
    }
    if (connected()) {
      Serial.println("BLE host disconnect timed out during unpair");
      success = false;
    }
  }

#if defined(CONFIG_BLUEDROID_ENABLED)
  int count = esp_ble_get_bond_device_num();
  if (count > 0) {
    auto* bonds = static_cast<esp_ble_bond_dev_t*>(
        calloc(static_cast<size_t>(count), sizeof(esp_ble_bond_dev_t)));
    if (bonds == nullptr) {
      success = false;
    } else {
      int listed = count;
      if (esp_ble_get_bond_device_list(&listed, bonds) != ESP_OK) {
        success = false;
      } else {
        for (int i = 0; i < listed; ++i) {
          if (esp_ble_remove_bond_device(bonds[i].bd_addr) != ESP_OK) {
            success = false;
          }
          delay(20);
        }
      }
      free(bonds);
    }
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) {
    success = false;
  } else {
    Serial.printf("BLE bonds before clear=%d\n", count);
    for (int i = 0; i < count; ++i) {
      if (ble_store_util_delete_peer(&peers[i]) != 0) {
        success = false;
      }
    }
    int remaining = 0;
    if (ble_store_util_bonded_peers(peers, &remaining,
                                    MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) {
      success = false;
    }
    Serial.printf("BLE bonds after clear=%d\n", remaining);
    success = success && remaining == 0;
  }
#else
  success = false;
#endif

  clearingBonds_ = false;
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising restarted after unpair");
  Serial.printf("BLE bonds clear %s\n", success ? "complete" : "failed");
  return success;
}

bool CodexMicroBle::connected() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool result = state_.connected;
  xSemaphoreGive(stateMutex_);
  return result;
}

CodexMicroState CodexMicroBle::snapshot() {
  CodexMicroState copy;
  if (stateMutex_ == nullptr) {
    return copy;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  copy = state_;
  state_.dirty = false;
  xSemaphoreGive(stateMutex_);
  return copy;
}

void CodexMicroBle::onConnected(bool connected) {
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.connected = connected;
  state_.secured = false;
  state_.ready = false;
  state_.diagnostic = connected ? "SECURING" : "";
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
  inputSubscribed_ = false;
  outputSeen_ = false;
  rpcBuffer_.clear();
  Serial.printf("BLE host %s\n", connected ? "connected" : "disconnected");
}

void CodexMicroBle::onSecurity(bool encrypted, bool authenticated, bool bonded,
                               bool authorized) {
  const bool secured = encrypted && bonded;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.secured = secured;
  state_.ready = secured && inputSubscribed_ && outputSeen_;
  state_.diagnostic = secured ? (state_.ready ? "" : "WAITING FOR CODEX")
                              : "PAIRING FAILED";
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
  if (!secured) {
    Serial.printf("BLE pairing failed encrypted=%u authenticated=%u bonded=%u authorized=%u\n",
                  encrypted, authenticated, bonded, authorized);
  }
}

void CodexMicroBle::onSubscribed(uint16_t value) {
  inputSubscribed_ = value != 0;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.ready = state_.secured && inputSubscribed_ && outputSeen_;
  state_.diagnostic = state_.ready ? "" :
      (state_.secured ? "WAITING FOR CODEX" : "SECURING");
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}

void CodexMicroBle::onNotifyStatus(int status, uint32_t code) {
  Serial.printf("BLE input notify status=%d code=%lu\n", status,
                static_cast<unsigned long>(code));
}

void CodexMicroBle::onOutput(const uint8_t* data, size_t length) {
  outputSeen_ = true;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.ready = state_.secured && inputSubscribed_;
  state_.diagnostic = state_.ready ? "" :
      (state_.secured ? "WAITING FOR SUBSCRIBE" : "SECURING");
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
  if (data == nullptr || length < 2) {
    return;
  }

  // HOGP normally strips the report ID. Accept an included ID as well so the
  // transport remains compatible with hosts that forward the raw report.
  size_t offset = (length >= 3 && data[0] == kReportId) ? 1 : 0;
  if (length < offset + 2 || data[offset] != 2) {
    return;
  }

  const size_t payloadLength = min<size_t>(data[offset + 1], kPayloadSize);
  if (length < offset + 2 + payloadLength) {
    return;
  }
  const char* payload = reinterpret_cast<const char*>(data + offset + 2);
  constexpr char kTopLevelPrefix[] = "{\"method\"";
  const bool startsTopLevel =
      payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
      memcmp(payload, kTopLevelPrefix, sizeof(kTopLevelPrefix) - 1) == 0;
  if (startsTopLevel && !rpcBuffer_.isEmpty()) {
    // A new top-level object means a previous fragmented write was dropped.
    // Resynchronize immediately instead of poisoning the next request.
    rpcBuffer_.clear();
  }
  if (rpcBuffer_.isEmpty()) {
    size_t jsonStart = 0;
    while (jsonStart < payloadLength && payload[jsonStart] != '{') {
      ++jsonStart;
    }
    if (jsonStart == payloadLength) {
      return;
    }
    rpcBuffer_.concat(payload + jsonStart, payloadLength - jsonStart);
  } else {
    rpcBuffer_.concat(payload, payloadLength);
  }

  DynamicJsonDocument request(4096);
  const DeserializationError error = deserializeJson(request, rpcBuffer_);
  if (error == DeserializationError::IncompleteInput) {
    return;
  }
  if (error) {
    Serial.printf("RPC parse error: %s\n", error.c_str());
    rpcBuffer_.clear();
    return;
  }

  handleRpc(request);
  rpcBuffer_.clear();
}

void CodexMicroBle::handleRpc(const JsonDocument& request) {
  const char* method = request["method"] | "";
  JsonVariantConst id = request["id"];
  JsonVariantConst params = request["params"];
  Serial.printf("RPC method=%s\n", method);

  if (strcmp(method, "sys.version") == 0) {
    StaticJsonDocument<128> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return;
  }

  if (strcmp(method, "device.status") == 0) {
    StaticJsonDocument<256> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    resultDoc["profile_index"] = 0;
    resultDoc["layer_index"] = 1;
    resultDoc["battery"] = batteryPercentage_;
    resultDoc["is_charging"] = charging_;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return;
  }

  if (strcmp(method, "v.oai.thstatus") == 0 && params.is<JsonArrayConst>()) {
    updateThreadLighting(params.as<JsonArrayConst>());
    sendSuccess(id);
    return;
  }

  if (strcmp(method, "v.oai.rgbcfg") == 0 && params.is<JsonObjectConst>()) {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    JsonObjectConst config = params.as<JsonObjectConst>();
    updateLightingSide(state_.ambient, config["ambient"].as<JsonObjectConst>());
    updateLightingSide(state_.keys, config["keys"].as<JsonObjectConst>());
    state_.dirty = true;
    xSemaphoreGive(stateMutex_);
    sendSuccess(id);
    return;
  }

  if (strcmp(method, "lights.preview") == 0 || strcmp(method, "host.focused_app") == 0) {
    sendSuccess(id);
    return;
  }

  StaticJsonDocument<192> response;
  response["id"] = id;
  JsonObject error = response.createNestedObject("error");
  error["code"] = -32601;
  error["message"] = "Method not found";
  String json;
  serializeJson(response, json);
  sendJson(json);
}

void CodexMicroBle::sendResult(JsonVariantConst id, JsonVariantConst result) {
  DynamicJsonDocument response(512);
  response["id"] = id;
  response["result"] = result;
  String json;
  serializeJson(response, json);
  sendJson(json);
}

void CodexMicroBle::sendSuccess(JsonVariantConst id) {
  StaticJsonDocument<96> resultDoc;
  resultDoc["ok"] = true;
  sendResult(id, resultDoc.as<JsonVariantConst>());
}

void CodexMicroBle::sendJson(const String& json) {
  if (input_ == nullptr) {
    return;
  }
  if (!connected()) {
    return;
  }

  String framed = json;
  framed += '\n';
  size_t offset = 0;
  while (offset < framed.length()) {
    const size_t chunk = min<size_t>(kPayloadSize, framed.length() - offset);
    uint8_t report[kReportBodySize] = {};
    report[0] = 2;
    report[1] = chunk;
    memcpy(report + 2, framed.c_str() + offset, chunk);
    input_->setValue(report, sizeof(report));
    input_->notify();
    offset += chunk;
    delay(4);
  }
}

void CodexMicroBle::updateThreadLighting(JsonArrayConst values) {
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  for (JsonObjectConst value : values) {
    const int id = value["id"] | -1;
    if (id < 0 || id >= static_cast<int>(state_.threads.size())) {
      continue;
    }
    ThreadLight& light = state_.threads[id];
    light.color = value["c"] | light.color;
    light.brightness = value["b"] | light.brightness;
    light.effect = value["e"] | light.effect;
    light.speed = value["s"] | light.speed;
  }
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}

void CodexMicroBle::updateLightingSide(LightingSide& side, JsonObjectConst value) {
  if (value.isNull()) {
    return;
  }
  side.color = value["c"] | side.color;
  side.brightness = value["b"] | side.brightness;
  side.effect = value["e"] | side.effect;
  side.speed = value["s"] | side.speed;
}
