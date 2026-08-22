// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLECharacteristic.h>
#include <BLEHIDDevice.h>
#include <BLEServer.h>
#include <freertos/queue.h>

#include <array>
#include <atomic>

struct ThreadLight {
  uint32_t color = 0;
  float brightness = 0.0f;
  String effect = "off";
  float speed = 0.0f;
};

struct LightingSide {
  uint32_t color = 0;
  float brightness = 0.0f;
  String effect = "off";
  float speed = 0.0f;
};

struct CodexMicroState {
  std::array<ThreadLight, 6> threads;
  std::array<uint32_t, 6> threadUpdateOrder{};
  LightingSide ambient;
  LightingSide keys;
  bool connected = false;
  bool secured = false;
  bool ready = false;
  bool standby = false;
  uint32_t threadRevision = 0;
  uint32_t threadUpdateSequence = 0;
  String diagnostic;
  bool dirty = true;
};

enum class BondClearResult : uint8_t { Success, Failure, RestartRequired };

class CodexMicroBle {
 public:
  static constexpr uint16_t kVendorId = 0x303A;
  static constexpr uint16_t kProductId = 0x8360;
  static constexpr uint8_t kReportId = 6;

  void begin();
  void setBattery(uint8_t percentage, bool charging);
  void sendKey(const char* key, uint8_t action, int8_t agent = -1);
  void sendJoystick(float angle, float distance);
  void maintain();
  BondClearResult clearBonds();
  bool connected();
  CodexMicroState snapshot();

 private:
  class ServerCallbacks;
  class InputCallbacks;
  class OutputCallbacks;
  friend class CodexSecurityCallbacks;

  void onConnected(bool connected);
  void onSecurity(bool encrypted, bool authenticated, bool bonded, bool authorized);
  void onSubscribed(uint16_t value);
  void onNotifyStatus(int status, uint32_t code);
#if defined(CODEX_BOARD_STICKS3)
  void queueOutput(const uint8_t* data, size_t length);
#endif
  void onOutput(const uint8_t* data, size_t length);
  void handleRpc(const JsonDocument& request);
  void sendResult(JsonVariantConst id, JsonVariantConst result);
  void sendSuccess(JsonVariantConst id);
  void sendJson(const String& json);
  void transmitJson(const char* json, size_t length);
  void updateThreadLighting(JsonArrayConst values);
  void updateLightingSide(LightingSide& side, JsonObjectConst value);

  BLEHIDDevice* hid_ = nullptr;
  BLEServer* server_ = nullptr;
  BLECharacteristic* input_ = nullptr;
  BLECharacteristic* output_ = nullptr;
  SemaphoreHandle_t stateMutex_ = nullptr;
#if defined(CODEX_BOARD_TAB5)
  static constexpr size_t kPendingMessageCapacity = 768;
  struct PendingMessage {
    uint16_t length = 0;
    char data[kPendingMessageCapacity] = {};
  };
  QueueHandle_t responseQueue_ = nullptr;
#endif
#if defined(CODEX_BOARD_STICKS3)
  struct PendingOutputReport {
    uint8_t length = 0;
    uint8_t data[64] = {};
  };
  QueueHandle_t outputQueue_ = nullptr;
#endif
  CodexMicroState state_;
  std::array<ThreadLight, 6> cachedVisibleThreads_;
  String rpcBuffer_;
  bool inputSubscribed_ = false;
  bool readySeen_ = false;
  bool cachedVisibleThreadsValid_ = false;
  std::atomic<bool> outputSeen_{false};
  std::atomic<uint32_t> lastValidRpcMs_{0};
  std::atomic<uint32_t> lastResponseMs_{0};
  std::atomic<uint32_t> notifySuccessCount_{0};
  std::atomic<uint32_t> notifyFailureCount_{0};
  uint8_t initializationMethods_ = 0;
  uint32_t initializationRetries_ = 0;
  uint8_t batteryPercentage_ = 100;
  bool charging_ = false;
  std::atomic<bool> clearingBonds_{false};
};
