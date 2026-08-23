// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace codex_micro {

enum class StickPage : uint8_t { Agents, Commands, Navigate, Config, Count };
enum class StickGesture : uint8_t { None, Next, Previous, NextPage };
enum class StickPageSelection : uint8_t { FirstAction, PreviousArrow, NextArrow };

class StickButtonController {
 public:
  StickButtonController(uint32_t doubleClickMs = 350, uint32_t holdMs = 500)
      : doubleClickMs_(doubleClickMs), holdMs_(holdMs) {}

  void pressed(uint32_t now) {
    pressed_ = true;
    held_ = false;
    pressedAt_ = now;
  }

  StickGesture update(uint32_t now) {
    if (pressed_ && !held_ && elapsed(now, pressedAt_) >= holdMs_) {
      held_ = true;
      pendingClick_ = false;
      return StickGesture::NextPage;
    }
    if (!pressed_ && pendingClick_ && elapsed(now, releasedAt_) > doubleClickMs_) {
      pendingClick_ = false;
      return StickGesture::Next;
    }
    return StickGesture::None;
  }

  StickGesture released(uint32_t now) {
    pressed_ = false;
    if (held_) {
      held_ = false;
      return StickGesture::None;
    }
    if (pendingClick_ && elapsed(now, releasedAt_) <= doubleClickMs_) {
      pendingClick_ = false;
      return StickGesture::Previous;
    }
    pendingClick_ = true;
    releasedAt_ = now;
    return StickGesture::None;
  }

 private:
  static uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }
  uint32_t doubleClickMs_;
  uint32_t holdMs_;
  uint32_t pressedAt_ = 0;
  uint32_t releasedAt_ = 0;
  bool pressed_ = false;
  bool held_ = false;
  bool pendingClick_ = false;
};

class StickUiController {
 public:
  StickPage page() const { return page_; }
  uint8_t selection() const { return selection_; }
  uint8_t selectedAgent() const { return selectedAgent_; }
  bool confirmingUnpair() const { return confirmingUnpair_; }
  bool confirmingTransport() const { return confirmingTransport_; }
  bool confirmingAction() const {
    return confirmingUnpair_ || confirmingTransport_;
  }

  uint8_t itemCount() const {
    if (confirmingAction()) return 2;
    switch (page_) {
      case StickPage::Agents: return 8;
      case StickPage::Commands: return 8;
      case StickPage::Navigate: return 9;
      case StickPage::Config: return 6;
      default: return 0;
    }
  }

  void move(int8_t delta) {
    const int count = itemCount();
    if (count == 0) return;
    int next = static_cast<int>(selection_) + delta;
    while (next < 0) next += count;
    selection_ = static_cast<uint8_t>(next % count);
    if (page_ == StickPage::Agents && selection_ < 6) selectedAgent_ = selection_;
  }

  void changePage(
      int8_t delta,
      StickPageSelection destination = StickPageSelection::FirstAction) {
    int next = static_cast<int>(page_) + delta;
    const int count = static_cast<int>(StickPage::Count);
    while (next < 0) next += count;
    page_ = static_cast<StickPage>(next % count);
    confirmingUnpair_ = false;
    confirmingTransport_ = false;
    if (destination == StickPageSelection::PreviousArrow) {
      selection_ = itemCount() - 2;
    } else if (destination == StickPageSelection::NextArrow) {
      selection_ = itemCount() - 1;
    } else {
      selection_ = page_ == StickPage::Agents ? selectedAgent_ : 0;
    }
  }

  void beginUnpairConfirmation() {
    confirmingUnpair_ = true;
    selection_ = 1;
  }
  void cancelUnpairConfirmation() {
    confirmingUnpair_ = false;
    page_ = StickPage::Config;
    selection_ = 0;
  }
  bool confirmsUnpair() const { return confirmingUnpair_ && selection_ == 1; }
  void beginTransportConfirmation() {
    confirmingTransport_ = true;
    selection_ = 1;
  }
  void cancelTransportConfirmation() {
    confirmingTransport_ = false;
    page_ = StickPage::Config;
    selection_ = 1;
  }
  bool confirmsTransport() const {
    return confirmingTransport_ && selection_ == 1;
  }

  void noteAgent(uint8_t agent) {
    if (agent < 6) {
      selectedAgent_ = agent;
      if (page_ == StickPage::Agents && !confirmingUnpair_) selection_ = agent;
    }
  }

 private:
  StickPage page_ = StickPage::Agents;
  uint8_t selection_ = 0;
  uint8_t selectedAgent_ = 0;
  bool confirmingUnpair_ = false;
  bool confirmingTransport_ = false;
};

class StickNotificationController {
 public:
  static constexpr size_t kAgentCount = 6;
  static constexpr uint8_t kActive = 1;
  static constexpr uint8_t kWaiting = 2;
  static constexpr uint8_t kComplete = 3;
  static constexpr uint8_t kError = 4;

  // Returns one notification request for the whole batch, even if several
  // agents enter attention states at the same time.
  bool update(const uint8_t* states, size_t count) {
    if (states == nullptr) return false;
    const size_t limit = count < kAgentCount ? count : kAgentCount;
    bool notify = false;
    for (size_t i = 0; i < limit; ++i) {
      const uint8_t current = states[i];
      if (!initialized_) {
        alerted_[i] = current != kActive;
      } else if (current == kActive) {
        alerted_[i] = false;
      } else if (isAttentionState(current) && current != previous_[i] &&
                 !alerted_[i]) {
        alerted_[i] = true;
        notify = true;
      }
      previous_[i] = current;
    }
    initialized_ = true;
    return notify;
  }

  bool initialized() const { return initialized_; }

 private:
  static bool isAttentionState(uint8_t state) {
    return state == kWaiting || state == kComplete || state == kError;
  }

  uint8_t previous_[kAgentCount] = {};
  bool alerted_[kAgentCount] = {};
  bool initialized_ = false;
};

class StickVolumeController {
 public:
  static constexpr uint8_t kMinimumLevel = 0;
  static constexpr uint8_t kMaximumLevel = 10;
  static constexpr uint8_t kDefaultLevel = 4;

  explicit StickVolumeController(uint8_t level = kDefaultLevel)
      : level_(clamp(level)) {}

  uint8_t level() const { return level_; }
  bool active() const { return level_ > kMinimumLevel; }
  uint8_t hardwareVolume() const { return toHardwareVolume(level_); }

  bool set(uint8_t level) {
    const uint8_t next = clamp(level);
    if (next == level_) return false;
    level_ = next;
    return true;
  }

  bool adjust(int8_t delta) {
    int next = static_cast<int>(level_) + delta;
    if (next < kMinimumLevel) next = kMinimumLevel;
    if (next > kMaximumLevel) next = kMaximumLevel;
    return set(static_cast<uint8_t>(next));
  }

  static uint8_t clamp(uint8_t level) {
    return level > kMaximumLevel ? kMaximumLevel : level;
  }

  static uint8_t toHardwareVolume(uint8_t level) {
    return static_cast<uint8_t>(clamp(level) * 255 / kMaximumLevel);
  }

 private:
  uint8_t level_;
};

inline bool stickShouldPlayNotification(bool requested, bool soundEnabled,
                                        bool speakerReady) {
  return requested && soundEnabled && speakerReady;
}

}  // namespace codex_micro
