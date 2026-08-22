// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

namespace codex_micro {

enum class StickPage : uint8_t { Agents, Commands, Navigate, Config, Count };
enum class StickGesture : uint8_t { None, Next, Previous, NextPage };

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

  uint8_t itemCount() const {
    if (confirmingUnpair_) return 2;
    switch (page_) {
      case StickPage::Agents: return 8;
      case StickPage::Commands: return 8;
      case StickPage::Navigate: return 9;
      case StickPage::Config: return 4;
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

  void changePage(int8_t delta) {
    int next = static_cast<int>(page_) + delta;
    const int count = static_cast<int>(StickPage::Count);
    while (next < 0) next += count;
    page_ = static_cast<StickPage>(next % count);
    selection_ = page_ == StickPage::Agents ? selectedAgent_ : 0;
    confirmingUnpair_ = false;
  }

  void beginUnpairConfirmation() {
    confirmingUnpair_ = true;
    selection_ = 0;
  }
  void cancelUnpairConfirmation() {
    confirmingUnpair_ = false;
    page_ = StickPage::Config;
    selection_ = 0;
  }
  bool confirmsUnpair() const { return confirmingUnpair_ && selection_ == 1; }

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
};

inline bool stickAgentUrgent(uint8_t visualState) {
  // Matches Waiting=2 and Error=4 in the firmware visual-state enum.
  return visualState == 2 || visualState == 4;
}

}  // namespace codex_micro
