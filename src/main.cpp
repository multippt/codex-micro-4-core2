// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#include <Arduino.h>
#include <M5Unified.h>
#if defined(CODEX_BOARD_TAB5)
#include <Preferences.h>
#include <esp_heap_caps.h>
#endif

#include <cmath>
#include <algorithm>
#include <array>

#include "BoardProfile.h"
#include "CodexMicroBle.h"

namespace {

enum class Page : uint8_t { Tasks, Commands, Control, Navigate };
enum class UnpairNotice : uint8_t { None, Success, Failure, Restarting };

struct TouchAction {
  const char* key = nullptr;
  int8_t agent = -1;
  bool encoderStep = false;
  bool joystick = false;
  float angle = 0.0f;

  TouchAction() = default;
  TouchAction(const char* keyValue, int8_t agentValue, bool encoderStepValue,
              bool joystickValue, float angleValue)
      : key(keyValue),
        agent(agentValue),
        encoderStep(encoderStepValue),
        joystick(joystickValue),
        angle(angleValue) {}
};

constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x18E3;
constexpr uint16_t kPanelPressed = 0x31A6;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kMuted = 0x9CF3;
constexpr uint16_t kAccent = 0x2E73;

struct Layout {
  int width;
  int height;
  int headerHeight;
  int tabHeight;
  int contentTop;
  int contentBottom;
  int margin;
  int gap;
  int textScale;
};

struct Rect {
  int x;
  int y;
  int width;
  int height;
};

enum class TaskVisualState : uint8_t { Idle, Active, Waiting, Complete, Error };

constexpr float kStandbyBrightness = 0.25f;

const char* kAgentKeys[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05"};
const char* kCommandKeys[] = {"ACT06", "ACT07", "ACT08", "ACT09", "ACT10", "ACT12"};
const char* kCommandLabels[] = {"FAST", "APPROVE", "DECLINE", "FORK", "MIC", "SEND"};
#if defined(CODEX_BOARD_TAB5)
constexpr uint8_t kControlCommandOrder[] = {0, 3, 1, 2, 4, 5};
constexpr char kPendingUnpairKey[] = "pending-unpair";
#endif

CodexMicroBle codex;
CodexMicroState state;
M5Canvas canvas(&M5.Display);
#if defined(CODEX_BOARD_TAB5)
Page page = Page::Control;
#else
Page page = Page::Tasks;
#endif
TouchAction activeAction;
bool touchActive = false;
uint32_t lastDrawMs = 0;
#if defined(CODEX_BOARD_TAB5)
bool drawPending = false;
uint16_t* regionBuffer = nullptr;
size_t regionBufferPixels = 0;
uint32_t renderTimingWindowMs = 0;
uint32_t fullRenderTotalUs = 0;
uint32_t tileRenderTotalUs = 0;
uint32_t displayTransferTotalUs = 0;
uint32_t fullRenderCount = 0;
uint32_t tileRenderCount = 0;
uint32_t displayTransferCount = 0;
#endif
uint32_t lastBatteryMs = 0;
Layout layout{};
bool unpairHolding = false;
bool unpairTriggered = false;
uint32_t unpairHoldStartMs = 0;
uint32_t unpairNoticeUntilMs = 0;
UnpairNotice unpairNotice = UnpairNotice::None;
#if defined(CODEX_BOARD_TAB5)
Preferences preferences;
bool soundEnabled = true;
bool speakerReady = false;
bool threadStateInitialized = false;
uint32_t lastThreadRevision = 0;
std::array<TaskVisualState, 6> previousTaskStates{};
std::array<bool, 6> attentionAlerted{};
int batteryLevel = -1;
bool externalPower = true;
bool batteryCharging = false;
bool batteryChargingInitialized = false;
bool pendingCharging = false;
uint8_t pendingChargingSamples = 0;
int pendingBatteryLevel = -1;
uint8_t pendingBatterySamples = 0;
bool bootUnpairPending = false;
#endif

constexpr uint32_t kUnpairHoldMs = 3000;
constexpr uint32_t kUnpairNoticeMs = 5000;
#if defined(CODEX_BOARD_TAB5)
constexpr uint8_t kSpeakerVolume = 96;
constexpr float kNotificationFrequency = 1000.0f;
constexpr uint32_t kNotificationDurationMs = 90;
constexpr int kExternalPowerCurrentThresholdMa = 2;
#endif

void updateLayout() {
  layout.width = M5.Display.width();
  layout.height = M5.Display.height();
  layout.headerHeight = max(30, layout.height / 8);
  layout.tabHeight = max(28, layout.height / 9);
  layout.contentTop = layout.headerHeight;
  layout.contentBottom = layout.height - layout.tabHeight;
  layout.margin = max(5, layout.width / 64);
  layout.gap = max(5, layout.width / 64);
  layout.textScale = max(1, layout.height / 240);
}

uint16_t rgb888To565(uint32_t color, float brightness = 1.0f) {
  const uint8_t red = ((color >> 16) & 0xFF) * brightness;
  const uint8_t green = ((color >> 8) & 0xFF) * brightness;
  const uint8_t blue = (color & 0xFF) * brightness;
  return canvas.color565(red, green, blue);
}

TaskVisualState taskVisualState(const ThreadLight& light) {
  if (light.brightness <= 0.01f) return TaskVisualState::Idle;
  const uint8_t red = (light.color >> 16) & 0xFF;
  const uint8_t green = (light.color >> 8) & 0xFF;
  const uint8_t blue = light.color & 0xFF;
  const uint8_t maximum = max(red, max(green, blue));
  const uint8_t minimum = min(red, min(green, blue));
  if (maximum - minimum < 40) return TaskVisualState::Idle;
  if (red > 180 && red > green * 3 / 2 && red > blue * 6 / 5) {
    return TaskVisualState::Error;
  }
  if (red > 160 && green > 100 && blue < 120) return TaskVisualState::Waiting;
  if (green > red * 6 / 5 && green > blue * 6 / 5) {
    return TaskVisualState::Complete;
  }
  if (blue > red * 6 / 5 && blue > green * 6 / 5) {
    return TaskVisualState::Active;
  }
  return TaskVisualState::Idle;
}

const char* taskStatusLabel(TaskVisualState status) {
  switch (status) {
    case TaskVisualState::Active: return "ACTIVE";
    case TaskVisualState::Waiting: return "WAITING";
    case TaskVisualState::Complete: return "COMPLETE";
    case TaskVisualState::Error: return "ERROR";
    case TaskVisualState::Idle: return "IDLE";
  }
  return "IDLE";
}

void drawCentered(const char* text, int x, int y, int font = 1, uint16_t color = kText) {
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(font);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

void formatConnectionStatus(char* status, size_t size) {
  if (unpairHolding) {
    const uint32_t elapsed = min<uint32_t>(kUnpairHoldMs, millis() - unpairHoldStartMs);
    snprintf(status, size, "UNPAIR %lu%%",
             static_cast<unsigned long>(elapsed * 100 / kUnpairHoldMs));
  } else if (unpairTriggered) {
    snprintf(status, size, "UNPAIRING");
  } else if (state.diagnostic == "PAIRING FAILED") {
    snprintf(status, size, "PAIR FAILED");
  } else if (state.standby) {
    snprintf(status, size, "STANDBY");
  } else {
#if defined(CODEX_BOARD_TAB5)
    snprintf(status, size, "%s", state.ready ? "LINK" : "PAIR");
#else
    snprintf(status, size, "%s", state.ready ? "LIVE" : "PAIR");
#endif
  }
}

#if defined(CODEX_BOARD_TAB5)
struct HeaderControlLayout {
  int pairLeft;
  int pairRight;
  int soundLeft;
  int soundRight;
};

HeaderControlLayout headerControlLayout(const char* status) {
  canvas.setTextSize(layout.textScale);
  const int statusWidth = canvas.textWidth(status);
  const int pairRight = layout.width - layout.margin;
  const int pairLeft = pairRight - max(150, statusWidth + 34 * layout.textScale);
  const int soundRight = pairLeft - layout.gap;
  const int soundLeft = soundRight - 150;
  return {pairLeft, pairRight, soundLeft, soundRight};
}
#endif

void drawHeader() {
  canvas.fillRect(0, 0, layout.width, layout.headerHeight, kBackground);
  canvas.setTextDatum(middle_left);
  canvas.setTextSize(2 * layout.textScale);
  canvas.setTextColor(kText);
  canvas.drawString("CODEX MICRO", layout.margin, layout.headerHeight / 2);

  canvas.setTextDatum(middle_right);
  canvas.setTextSize(layout.textScale);
  canvas.setTextColor(kMuted);
  char status[24];
  formatConnectionStatus(status, sizeof(status));
  const uint16_t dot = state.ready ? 0x07E0 : (state.connected ? 0xFFE0 : 0xF800);
  const int statusWidth = canvas.textWidth(status);
#if defined(CODEX_BOARD_TAB5)
  const HeaderControlLayout controls = headerControlLayout(status);
  const int pairRight = controls.pairRight;
  const int dotX = pairRight - statusWidth - 8 * layout.textScale;
#else
  const int pairRight = layout.width - layout.margin;
  const int pairLeft = layout.width * 2 / 3;
  const int dotX = pairRight - statusWidth - 8 * layout.textScale;
#endif
  canvas.fillCircle(dotX, layout.headerHeight / 2, 4 * layout.textScale, dot);
  canvas.drawString(status, pairRight, layout.headerHeight / 2);

#if defined(CODEX_BOARD_TAB5)
  const int soundRight = controls.soundRight;
  const int soundLeft = controls.soundLeft;
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(soundEnabled ? kText : kMuted);
  canvas.drawString(soundEnabled ? "SOUND" : "MUTE",
                    (soundLeft + soundRight) / 2, layout.headerHeight / 2);

  char batteryText[16];
  if (externalPower) {
    snprintf(batteryText, sizeof(batteryText), "USB");
  } else {
    snprintf(batteryText, sizeof(batteryText), "%d%%%s", batteryLevel,
             batteryCharging ? "+" : "");
  }
  const int batteryRight = soundLeft - 2 * layout.gap;
  const int batteryTextWidth = canvas.textWidth(batteryText);
  const int iconWidth = 50;
  const int iconHeight = 24;
  const int iconRight = batteryRight - batteryTextWidth - layout.gap;
  const int iconLeft = iconRight - iconWidth;
  const int iconTop = (layout.headerHeight - iconHeight) / 2;
  canvas.drawRect(iconLeft, iconTop, iconWidth, iconHeight, kMuted);
  canvas.fillRect(iconRight, iconTop + 7, 5, iconHeight - 14, kMuted);
  if (externalPower) {
    const int centerX = iconLeft + iconWidth / 2;
    const int centerY = iconTop + iconHeight / 2;
    const int boltWidth = 5 * layout.textScale;
    const int boltHeight = 5 * layout.textScale;
    canvas.fillTriangle(centerX + boltWidth / 3, centerY - boltHeight,
                        centerX - boltWidth, centerY + 1,
                        centerX, centerY + 1, kText);
    canvas.fillTriangle(centerX - boltWidth / 3, centerY + boltHeight,
                        centerX + boltWidth, centerY - 1,
                        centerX, centerY - 1, kText);
  } else if (batteryLevel >= 0) {
    const int fillWidth = (iconWidth - 6) * min(100, batteryLevel) / 100;
    const uint16_t batteryColor = batteryLevel <= 15 ? 0xF800
                                    : (batteryCharging ? 0x07E0 : kAccent);
    canvas.fillRect(iconLeft + 3, iconTop + 3, fillWidth, iconHeight - 6,
                    batteryColor);
  }
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(kMuted);
  canvas.drawString(batteryText, batteryRight, layout.headerHeight / 2);
#endif

  if (unpairHolding) {
    const int barWidth = layout.width / 3;
    const int filled = static_cast<int>(barWidth *
        min<uint32_t>(kUnpairHoldMs, millis() - unpairHoldStartMs) / kUnpairHoldMs);
    canvas.fillRect(layout.width - barWidth, layout.headerHeight - 3 * layout.textScale,
                    filled, 3 * layout.textScale, kAccent);
  }
}

void drawUnpairNotice() {
  if (unpairNotice == UnpairNotice::None) return;
  const int width = layout.width * 4 / 5;
  const int height = max(70, layout.height / 4);
  const int x = (layout.width - width) / 2;
  const int y = (layout.height - height) / 2;
  canvas.fillRoundRect(x, y, width, height, 8 * layout.textScale, kPanel);
  const uint16_t noticeColor = unpairNotice == UnpairNotice::Success ? 0x07E0
                               : (unpairNotice == UnpairNotice::Restarting
                                      ? kAccent
                                      : 0xF800);
  canvas.drawRoundRect(x, y, width, height, 8 * layout.textScale, noticeColor);
  if (unpairNotice == UnpairNotice::Success) {
    drawCentered("UNPAIRED", layout.width / 2, y + height * 2 / 5,
                 2 * layout.textScale, kText);
    drawCentered("FORGET ON HOST", layout.width / 2, y + height * 3 / 5,
                 layout.textScale, kMuted);
  } else if (unpairNotice == UnpairNotice::Restarting) {
    drawCentered("RESTARTING BLE", layout.width / 2, y + height / 2,
                 2 * layout.textScale, kAccent);
  } else {
    drawCentered("UNPAIR FAILED", layout.width / 2, y + height / 2,
                 2 * layout.textScale, 0xF800);
  }
}

void drawTabs() {
#if defined(CODEX_BOARD_TAB5)
  const char* labels[] = {"CONTROL", "NAVIGATE"};
  const Page pages[] = {Page::Control, Page::Navigate};
  constexpr int tabCount = 2;
#else
  const char* labels[] = {"TASKS", "COMMANDS", "NAVIGATE"};
  const Page pages[] = {Page::Tasks, Page::Commands, Page::Navigate};
  constexpr int tabCount = 3;
#endif
  for (int i = 0; i < tabCount; ++i) {
    const int x = i * layout.width / tabCount;
    const int width = (i + 1) * layout.width / tabCount - x;
    const bool selected = page == pages[i];
    canvas.fillRect(x, layout.contentBottom, width, layout.tabHeight,
                    selected ? kAccent : kPanel);
    drawCentered(labels[i], x + width / 2, layout.contentBottom + layout.tabHeight / 2,
                 layout.textScale,
                 selected ? kText : kMuted);
  }
}

Rect contentBounds() {
  return {layout.margin, layout.contentTop + layout.margin,
          layout.width - 2 * layout.margin,
          layout.contentBottom - layout.contentTop - 2 * layout.margin};
}

Rect gridButtonRect(const Rect& bounds, int index, int columns = 3) {
  const int rows = (6 + columns - 1) / columns;
  const int row = index / columns;
  const int col = index % columns;
  const int buttonWidth = (bounds.width - (columns - 1) * layout.gap) / columns;
  const int buttonHeight = (bounds.height - (rows - 1) * layout.gap) / rows;
  return {bounds.x + col * (buttonWidth + layout.gap),
          bounds.y + row * (buttonHeight + layout.gap),
          buttonWidth, buttonHeight};
}

void drawButton(int x, int y, int width, int height, const char* label, uint16_t border,
                bool pressed = false, const char* sublabel = nullptr,
                bool spacious = false, int borderWidth = 1,
                uint16_t labelColor = kText, uint16_t sublabelColor = kMuted) {
  canvas.fillRoundRect(x, y, width, height, 6, pressed ? kPanelPressed : kPanel);
  for (int inset = 0; inset < borderWidth; ++inset) {
    canvas.drawRoundRect(x + inset, y + inset, width - 2 * inset,
                         height - 2 * inset, max(1, 6 - inset), border);
  }
  const int labelY = spacious && sublabel ? y + height * 42 / 100
                                          : y + height / 2 - (sublabel ? 7 : 0);
  const int sublabelY = spacious ? y + height * 72 / 100
                                 : y + height / 2 + 13 * layout.textScale;
  drawCentered(label, x + width / 2, labelY,
               (strlen(label) > 7 ? 1 : 2) * layout.textScale, labelColor);
  if (sublabel != nullptr) {
    drawCentered(sublabel, x + width / 2, sublabelY,
                 layout.textScale, sublabelColor);
  }
}

void drawTask(const Rect& bounds, int i, int columns = 3, bool spacious = false) {
  const Rect button = gridButtonRect(bounds, i, columns);
  const ThreadLight& light = state.threads[i];
  const TaskVisualState visualState = taskVisualState(light);
  const float displayBrightness =
      light.brightness * (state.standby ? kStandbyBrightness : 1.0f);
  float pulse = 1.0f;
  if (!state.standby && light.effect == "breath") {
    pulse = 0.55f + 0.45f * (std::sin(millis() * 0.006f) * 0.5f + 0.5f);
  } else if (!state.standby && visualState == TaskVisualState::Error) {
    pulse = 0.35f + 0.65f * (std::sin(millis() * 0.012f) * 0.5f + 0.5f);
  }
  const uint16_t baseColor = light.brightness <= 0.01f
                                 ? 0x8410
                                 : rgb888To565(light.color, displayBrightness);
  const uint16_t color = light.brightness <= 0.01f
                             ? 0x4208
                             : rgb888To565(light.color, displayBrightness * pulse);
  char title[12];
  snprintf(title, sizeof(title), "AGENT %d", i + 1);
  const char* status = taskStatusLabel(visualState);
  const bool pressed = touchActive && activeAction.agent == i;
  drawButton(button.x, button.y, button.width, button.height, title, color,
             pressed, status, spacious, spacious ? 5 : 1,
             spacious ? baseColor : kText,
             spacious ? baseColor : kMuted);
  if (spacious) {
    canvas.fillCircle(button.x + 12 * layout.textScale,
                      button.y + button.height * 72 / 100,
                      3 * layout.textScale, color);
  } else {
    canvas.fillCircle(button.x + button.width - 12 * layout.textScale,
                      button.y + 12 * layout.textScale,
                      4 * layout.textScale, color);
  }
}

void drawTasks(const Rect& bounds, int columns = 3, bool spacious = false) {
  for (int i = 0; i < 6; ++i) drawTask(bounds, i, columns, spacious);
}

void drawCommands(const Rect& bounds, int columns = 3, bool controlOrder = false,
                  bool spacious = false) {
  for (int position = 0; position < 6; ++position) {
#if defined(CODEX_BOARD_TAB5)
    const int i = controlOrder ? kControlCommandOrder[position] : position;
#else
    const int i = position;
#endif
    const Rect button = gridButtonRect(bounds, position, columns);
    const bool pressed = touchActive && activeAction.key == kCommandKeys[i];
    const char* hint = i == 4 ? "HOLD / 2X" : "TAP";
    const uint16_t border = i == 1 ? 0x07E0
                                   : (controlOrder && i == 2 ? 0xF9A6 : kAccent);
    drawButton(button.x, button.y, button.width, button.height,
               kCommandLabels[i], border, pressed, hint, spacious,
               spacious ? 2 : 1);
  }
}

#if defined(CODEX_BOARD_TAB5)
void controlBounds(Rect& tasks, Rect& commands) {
  const Rect content = contentBounds();
  const int sectionTitleHeight = max(28, 18 * layout.textScale);
  const int sectionWidth = (content.width - layout.gap) / 2;
  tasks = {content.x, content.y + sectionTitleHeight, sectionWidth,
           content.height - sectionTitleHeight};
  commands = {content.x + sectionWidth + layout.gap,
              content.y + sectionTitleHeight,
              content.width - sectionWidth - layout.gap,
              content.height - sectionTitleHeight};
}

void drawControl() {
  Rect tasks;
  Rect commands;
  controlBounds(tasks, commands);
  const int titleY = contentBounds().y + (tasks.y - contentBounds().y) / 2;
  drawCentered("TASKS", tasks.x + tasks.width / 2, titleY,
               layout.textScale, kMuted);
  drawCentered("COMMANDS", commands.x + commands.width / 2, titleY,
               layout.textScale, kMuted);
  drawTasks(tasks, 2, true);
  drawCommands(commands, 2, true, true);
}
#endif

void drawNavigate() {
  const int top = layout.contentTop + layout.margin;
  const int availableHeight = layout.contentBottom - top - layout.margin;
  const int leftWidth = (layout.width - 3 * layout.margin) * 46 / 100;
  const int rightX = 2 * layout.margin + leftWidth;
  const int rightWidth = layout.width - rightX - layout.margin;
  const int dpadWidth = leftWidth * 48 / 100;
  const int dpadHeight = availableHeight * 30 / 100;
  const int centerY = top + availableHeight / 2;
  drawButton(layout.margin + dpadWidth / 2, top, dpadWidth, dpadHeight, "UP", kAccent,
             touchActive && activeAction.joystick && activeAction.angle == 0.75f);
  drawButton(layout.margin + dpadWidth / 2, top + availableHeight - dpadHeight,
             dpadWidth, dpadHeight, "DOWN", kAccent,
             touchActive && activeAction.joystick && activeAction.angle == 0.25f);
  drawButton(layout.margin, centerY - dpadHeight / 2, dpadWidth, dpadHeight, "LEFT", kAccent,
             touchActive && activeAction.joystick && activeAction.angle == 0.5f);
  drawButton(layout.margin + dpadWidth, centerY - dpadHeight / 2, dpadWidth, dpadHeight,
             "RIGHT", kAccent,
             touchActive && activeAction.joystick && activeAction.angle == 0.0f);

  const int encoderGap = layout.gap;
  const int encoderWidth = (rightWidth - encoderGap) / 2;
  const int encoderHeight = availableHeight * 38 / 100;
  drawButton(rightX, top, encoderWidth, encoderHeight, "CCW", 0xFFE0,
             touchActive && activeAction.key != nullptr && strcmp(activeAction.key, "ENC_CC") == 0,
             "NEXT");
  drawButton(rightX + encoderWidth + encoderGap, top, encoderWidth, encoderHeight, "CW", 0xFFE0,
             touchActive && activeAction.key != nullptr && strcmp(activeAction.key, "ENC_CW") == 0,
             "PREV");
  drawButton(rightX, top + encoderHeight + layout.gap, rightWidth,
             availableHeight - encoderHeight - layout.gap, "DIAL", 0xFFE0,
             touchActive && activeAction.key != nullptr && strcmp(activeAction.key, "ENC") == 0,
             "TAP / HOLD SETTINGS");
}

void renderScreen() {
  canvas.fillScreen(kBackground);
  drawHeader();
  switch (page) {
    case Page::Tasks:
      drawTasks(contentBounds());
      break;
    case Page::Commands:
      drawCommands(contentBounds());
      break;
    case Page::Control:
#if defined(CODEX_BOARD_TAB5)
      drawControl();
#endif
      break;
    case Page::Navigate:
      drawNavigate();
      break;
  }
  drawTabs();
  drawUnpairNotice();
}

void drawScreen() {
  const uint32_t renderStartUs = micros();
  renderScreen();
  const uint32_t transferStartUs = micros();
  canvas.pushSprite(0, 0);
  const uint32_t transferEndUs = micros();
  lastDrawMs = millis();
#if defined(CODEX_BOARD_TAB5)
  fullRenderTotalUs += transferStartUs - renderStartUs;
  ++fullRenderCount;
  displayTransferTotalUs += transferEndUs - transferStartUs;
  ++displayTransferCount;
  drawPending = false;
#endif
}

#if defined(CODEX_BOARD_TAB5)
bool pushCanvasRegion(const Rect& region) {
  const size_t pixels = static_cast<size_t>(region.width) * region.height;
  if (pixels > regionBufferPixels) {
    auto* resized = static_cast<uint16_t*>(heap_caps_realloc(
        regionBuffer, pixels * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (resized == nullptr) return false;
    regionBuffer = resized;
    regionBufferPixels = pixels;
  }
  const auto* source = static_cast<const uint16_t*>(canvas.getBuffer());
  for (int row = 0; row < region.height; ++row) {
    memcpy(regionBuffer + static_cast<size_t>(row) * region.width,
           source + static_cast<size_t>(region.y + row) * layout.width + region.x,
           static_cast<size_t>(region.width) * sizeof(uint16_t));
  }
  M5.Display.pushImage(region.x, region.y, region.width, region.height,
                       regionBuffer);
  return true;
}

Rect taskButtonRect(int agent) {
  Rect tasks;
  Rect commands;
  controlBounds(tasks, commands);
  return gridButtonRect(tasks, agent, 2);
}

void redrawTaskRegions(const std::array<bool, 6>& redraw) {
  Rect tasks;
  Rect commands;
  controlBounds(tasks, commands);
  for (int i = 0; i < 6; ++i) {
    if (!redraw[i]) continue;
    const uint32_t renderStartUs = micros();
    drawTask(tasks, i, 2, true);
    const uint32_t transferStartUs = micros();
    if (!pushCanvasRegion(taskButtonRect(i))) {
      drawScreen();
      return;
    }
    const uint32_t transferEndUs = micros();
    tileRenderTotalUs += transferStartUs - renderStartUs;
    ++tileRenderCount;
    displayTransferTotalUs += transferEndUs - transferStartUs;
    ++displayTransferCount;
  }
  lastDrawMs = millis();
  drawPending = false;
}

void reportRenderTimings() {
  const uint32_t now = millis();
  if (renderTimingWindowMs == 0) renderTimingWindowMs = now;
  if (now - renderTimingWindowMs < 5000) return;
  Serial.printf(
      "Render timing full=%lu/%luus tile=%lu/%luus transfer=%lu/%luus\n",
      static_cast<unsigned long>(fullRenderCount),
      static_cast<unsigned long>(
          fullRenderCount ? fullRenderTotalUs / fullRenderCount : 0),
      static_cast<unsigned long>(tileRenderCount),
      static_cast<unsigned long>(
          tileRenderCount ? tileRenderTotalUs / tileRenderCount : 0),
      static_cast<unsigned long>(displayTransferCount),
      static_cast<unsigned long>(displayTransferCount
                                     ? displayTransferTotalUs / displayTransferCount
                                     : 0));
  renderTimingWindowMs = now;
  fullRenderTotalUs = tileRenderTotalUs = displayTransferTotalUs = 0;
  fullRenderCount = tileRenderCount = displayTransferCount = 0;
}

void redrawTaskRegion(int agent) {
  std::array<bool, 6> redraw{};
  if (agent >= 0 && agent < 6) redraw[agent] = true;
  redrawTaskRegions(redraw);
}
#endif

void requestDraw() {
#if defined(CODEX_BOARD_TAB5)
  drawPending = true;
#else
  drawScreen();
#endif
}

bool inRect(int x, int y, int left, int top, int width, int height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

bool isUnpairTarget(int x, int y) {
#if defined(CODEX_BOARD_TAB5)
  char status[24];
  formatConnectionStatus(status, sizeof(status));
  const HeaderControlLayout controls = headerControlLayout(status);
  return y >= 0 && y < layout.headerHeight && x >= controls.pairLeft;
#else
  return y >= 0 && y < layout.headerHeight && x >= layout.width * 2 / 3;
#endif
}

#if defined(CODEX_BOARD_TAB5)
bool isSoundTarget(int x, int y) {
  char status[24];
  formatConnectionStatus(status, sizeof(status));
  const HeaderControlLayout controls = headerControlLayout(status);
  return y >= 0 && y < layout.headerHeight &&
         x >= controls.soundLeft && x < controls.soundRight;
}

void toggleSound() {
  soundEnabled = !soundEnabled;
  preferences.begin("codex-micro", false);
  preferences.putBool("sound", soundEnabled);
  preferences.end();
  Serial.printf("Speaker notifications %s\n", soundEnabled ? "enabled" : "muted");
  requestDraw();
}

void processThreadNotifications(const CodexMicroState& latest) {
  if (latest.threadRevision == lastThreadRevision) return;
  bool notify = false;
  std::array<TaskVisualState, 6> current{};
  for (size_t i = 0; i < current.size(); ++i) {
    current[i] = taskVisualState(latest.threads[i]);
    if (!threadStateInitialized) {
      attentionAlerted[i] = current[i] != TaskVisualState::Active;
      continue;
    }
    if (current[i] == TaskVisualState::Active) {
      attentionAlerted[i] = false;
    } else if (current[i] != previousTaskStates[i] && !attentionAlerted[i]) {
      attentionAlerted[i] = true;
      notify = true;
    }
  }
  previousTaskStates = current;
  lastThreadRevision = latest.threadRevision;
  if (!threadStateInitialized) {
    threadStateInitialized = true;
    Serial.println("Agent notification baseline synchronized");
    return;
  }
  if (notify) {
    Serial.printf("Agent attention transition notification (%s)\n",
                  soundEnabled ? "beep" : "muted");
    if (soundEnabled && speakerReady) {
      M5.Speaker.tone(kNotificationFrequency, kNotificationDurationMs);
    }
  }
}
#endif

void startUnpairHold() {
  unpairHolding = true;
  unpairTriggered = false;
  unpairHoldStartMs = millis();
  requestDraw();
}

void finishUnpairHold() {
  unpairHolding = false;
  unpairTriggered = true;
  drawScreen();
  const BondClearResult result = codex.clearBonds();
#if defined(CODEX_BOARD_TAB5)
  if (result == BondClearResult::RestartRequired) {
    preferences.begin("codex-micro", false);
    const bool recoveryPersisted =
        preferences.putBool(kPendingUnpairKey, true) == sizeof(bool);
    preferences.end();
    if (recoveryPersisted) {
      unpairNotice = UnpairNotice::Restarting;
      drawScreen();
      delay(750);
      ESP.restart();
      return;
    }
    Serial.println("Failed to persist pending Tab5 unpair recovery");
  }
#endif
  unpairNotice = result == BondClearResult::Success ? UnpairNotice::Success
                                                    : UnpairNotice::Failure;
  unpairNoticeUntilMs = millis() + kUnpairNoticeMs;
  drawScreen();
}

TouchAction actionAt(int x, int y) {
  if (y >= layout.contentBottom) {
#if defined(CODEX_BOARD_TAB5)
    page = x < layout.width / 2 ? Page::Control : Page::Navigate;
#else
    const Page pages[] = {Page::Tasks, Page::Commands, Page::Navigate};
    page = pages[min(2, x * 3 / layout.width)];
#endif
    drawScreen();
    return {};
  }

  Rect taskBounds = contentBounds();
  Rect commandBounds = contentBounds();
#if defined(CODEX_BOARD_TAB5)
  if (page == Page::Control) controlBounds(taskBounds, commandBounds);
#endif
  if (page == Page::Tasks || page == Page::Control) {
    for (int i = 0; i < 6; ++i) {
      const int columns = page == Page::Control ? 2 : 3;
      const Rect button = gridButtonRect(taskBounds, i, columns);
      if (inRect(x, y, button.x, button.y, button.width, button.height)) {
        return {kAgentKeys[i], static_cast<int8_t>(i), false, false, 0.0f};
      }
    }
  }
  if (page == Page::Commands || page == Page::Control) {
    for (int position = 0; position < 6; ++position) {
      const int columns = page == Page::Control ? 2 : 3;
      const Rect button = gridButtonRect(commandBounds, position, columns);
      if (inRect(x, y, button.x, button.y, button.width, button.height)) {
#if defined(CODEX_BOARD_TAB5)
        const int i = page == Page::Control ? kControlCommandOrder[position]
                                            : position;
#else
        const int i = position;
#endif
        return {kCommandKeys[i], -1, false, false, 0.0f};
      }
    }
  }
  if (page == Page::Navigate) {
    const int top = layout.contentTop + layout.margin;
    const int ah = layout.contentBottom - top - layout.margin;
    const int lw = (layout.width - 3 * layout.margin) * 46 / 100;
    const int rx = 2 * layout.margin + lw;
    const int rw = layout.width - rx - layout.margin;
    const int dw = lw * 48 / 100;
    const int dh = ah * 30 / 100;
    const int cy = top + ah / 2;
    const int ew = (rw - layout.gap) / 2;
    const int eh = ah * 38 / 100;
    if (inRect(x, y, layout.margin + dw / 2, top, dw, dh)) return {nullptr, -1, false, true, 0.75f};
    if (inRect(x, y, layout.margin + dw / 2, top + ah - dh, dw, dh)) return {nullptr, -1, false, true, 0.25f};
    if (inRect(x, y, layout.margin, cy - dh / 2, dw, dh)) return {nullptr, -1, false, true, 0.5f};
    if (inRect(x, y, layout.margin + dw, cy - dh / 2, dw, dh)) return {nullptr, -1, false, true, 0.0f};
    if (inRect(x, y, rx, top, ew, eh)) return {"ENC_CC", -1, true, false, 0.0f};
    if (inRect(x, y, rx + ew + layout.gap, top, ew, eh)) return {"ENC_CW", -1, true, false, 0.0f};
    if (inRect(x, y, rx, top + eh + layout.gap, rw, ah - eh - layout.gap)) return {"ENC", -1, false, false, 0.0f};
  }
  return {};
}

void pressAction(const TouchAction& action) {
  activeAction = action;
  touchActive = action.key != nullptr || action.joystick;
  if (!touchActive) return;

  if (action.joystick) {
    codex.sendJoystick(action.angle, 1.0f);
  } else if (action.encoderStep) {
    codex.sendKey(action.key, 2);
  } else {
    codex.sendKey(action.key, 1, action.agent);
  }
#if defined(CODEX_BOARD_TAB5)
  if (page == Page::Control && action.agent >= 0) {
    redrawTaskRegion(action.agent);
  } else {
    requestDraw();
  }
#else
  requestDraw();
#endif
}

void releaseAction() {
  if (!touchActive) return;
  const int8_t releasedAgent = activeAction.agent;
  if (activeAction.joystick) {
    codex.sendJoystick(activeAction.angle, 0.0f);
  } else if (!activeAction.encoderStep && activeAction.key != nullptr) {
    codex.sendKey(activeAction.key, 0, activeAction.agent);
  }
  touchActive = false;
  activeAction = {};
#if defined(CODEX_BOARD_TAB5)
  if (page == Page::Control && releasedAgent >= 0) {
    redrawTaskRegion(releasedAgent);
  } else {
    requestDraw();
  }
#else
  requestDraw();
#endif
}

void updateBattery() {
  if (millis() - lastBatteryMs < 30000 && lastBatteryMs != 0) return;
  lastBatteryMs = millis();
#if defined(CODEX_BOARD_TAB5)
  // Tab5's M5Unified percentage is calculated from one INA226 read. A
  // transient failed I2C transaction can therefore look like 0%, while an
  // outlier clamps to 100%. Median sampling removes isolated failures.
  std::array<int, 7> levels{};
  size_t validLevelCount = 0;
  for (size_t i = 0; i < levels.size(); ++i) {
    const int level = M5.Power.getBatteryLevel();
    // On a powered Tab5, an exact zero is the INA226 failure/clamp observed
    // in M5Unified rather than a useful remaining-capacity measurement.
    if (level > 0 && level <= 100) levels[validLevelCount++] = level;
    delay(2);
  }

  int sampledLevel = -1;
  int minimumLevel = -1;
  int maximumLevel = -1;
  if (validLevelCount >= 4) {
    std::sort(levels.begin(), levels.begin() + validLevelCount);
    minimumLevel = levels[0];
    maximumLevel = levels[validLevelCount - 1];
    sampledLevel = levels[validLevelCount / 2];
  }

  const int batteryVoltageMv = M5.Power.getBatteryVoltage();
  const int batteryCurrentMa = M5.Power.getBatteryCurrent();
  const bool sampledCharging = M5.Power.isCharging();
  const bool previousCharging = batteryCharging;
  if (!batteryChargingInitialized) {
    batteryCharging = sampledCharging;
    batteryChargingInitialized = true;
  } else if (sampledCharging == batteryCharging) {
    pendingChargingSamples = 0;
  } else if (sampledCharging != pendingCharging) {
    pendingCharging = sampledCharging;
    pendingChargingSamples = 1;
  } else if (++pendingChargingSamples >= 2) {
    batteryCharging = sampledCharging;
    pendingChargingSamples = 0;
  }

  const int previousLevel = batteryLevel;
  if (sampledLevel < 0) {
    pendingBatterySamples = 0;
  } else if (batteryLevel < 0 || abs(sampledLevel - batteryLevel) <= 15) {
    batteryLevel = sampledLevel;
    pendingBatterySamples = 0;
  } else if (pendingBatteryLevel < 0 ||
             abs(sampledLevel - pendingBatteryLevel) > 3) {
    pendingBatteryLevel = sampledLevel;
    pendingBatterySamples = 1;
  } else if (++pendingBatterySamples >= 3) {
    batteryLevel = sampledLevel;
    pendingBatterySamples = 0;
  }
  const bool previousExternalPower = externalPower;
  const bool fullScaleWithoutBatteryCurrent =
      sampledLevel == 100 && batteryVoltageMv >= 8300 &&
      abs(batteryCurrentMa) <= kExternalPowerCurrentThresholdMa;
  externalPower = batteryLevel < 0 || fullScaleWithoutBatteryCurrent;
  const bool changed = batteryLevel != previousLevel ||
                       externalPower != previousExternalPower;
  Serial.printf("Battery samples=%u range=%d..%d median=%d level=%d voltage=%dmV "
                "current=%dmA charging=%s power=%s%s\n",
                static_cast<unsigned>(validLevelCount), minimumLevel,
                maximumLevel, sampledLevel,
                batteryLevel, batteryVoltageMv, batteryCurrentMa,
                batteryCharging ? "yes" : "no",
                externalPower ? "usb" : "battery",
                sampledLevel < 0 ? " retained" : "");
  if ((changed || batteryCharging != previousCharging) &&
      canvas.getBuffer() != nullptr) {
    drawScreen();
  }
  if (externalPower) {
    codex.setBattery(100, false);
  } else if (batteryLevel >= 0) {
    codex.setBattery(static_cast<uint8_t>(batteryLevel), batteryCharging);
  }
#else
  const int level = M5.Power.getBatteryLevel();
  const bool charging = M5.Power.isCharging();
  codex.setBattery(level < 0 ? 100 : static_cast<uint8_t>(level), charging);
#endif
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(kBootMessage);
  Serial.printf("Board profile: %s, firmware: %s\n", kBoardName, kFirmwareVersion);

  auto config = M5.config();
  config.clear_display = true;
  M5.begin(config);
#if defined(CODEX_BOARD_TAB5)
  preferences.begin("codex-micro", false);
  soundEnabled = preferences.getBool("sound", true);
  bootUnpairPending = preferences.getBool(kPendingUnpairKey, false);
  if (bootUnpairPending) preferences.remove(kPendingUnpairKey);
  preferences.end();
  speakerReady = M5.Speaker.begin();
  if (speakerReady) M5.Speaker.setVolume(kSpeakerVolume);
  Serial.printf("Tab5 speaker initialization %s, notifications %s\n",
                speakerReady ? "complete" : "failed",
                soundEnabled ? "enabled" : "muted");
#endif
  M5.Display.setRotation(kDisplayRotation);
  M5.Display.setBrightness(kDisplayBrightness);
  M5.Display.setTextWrap(false);
  updateLayout();

  canvas.setColorDepth(16);
  if (canvas.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Canvas allocation failed", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    Serial.println("Canvas allocation failed");
    while (true) delay(1000);
  }
  canvas.setTextWrap(false);

  canvas.fillScreen(kBackground);
  drawCentered("STARTING BLE", layout.width / 2, layout.height / 2,
               layout.textScale, kMuted);
  canvas.pushSprite(0, 0);
  Serial.println("Starting BLE (Tab5 uses the ESP32-C6 hosted controller)");
  codex.begin();
  state = codex.snapshot();
#if defined(CODEX_BOARD_TAB5)
  if (bootUnpairPending) {
    Serial.println("Retrying pending Tab5 bond clear after automatic restart");
    const BondClearResult result = codex.clearBonds();
    unpairNotice = result == BondClearResult::Success ? UnpairNotice::Success
                                                      : UnpairNotice::Failure;
    unpairNoticeUntilMs = millis() + kUnpairNoticeMs;
  }
#endif
  updateBattery();
  drawScreen();
  Serial.println("CODEX_MICRO_READY");
}

void loop() {
  M5.update();
  codex.maintain();
  const auto touch = M5.Touch.getDetail();
  if (touch.wasPressed()) {
    if (!unpairTriggered && isUnpairTarget(touch.x, touch.y)) {
      startUnpairHold();
#if defined(CODEX_BOARD_TAB5)
    } else if (!unpairTriggered && isSoundTarget(touch.x, touch.y)) {
      toggleSound();
#endif
    } else if (!unpairTriggered) {
      pressAction(actionAt(touch.x, touch.y));
    }
  }
  if (touch.wasReleased()) {
    if (unpairHolding && millis() - unpairHoldStartMs >= kUnpairHoldMs) {
      finishUnpairHold();
      unpairTriggered = false;
    } else if (unpairHolding || unpairTriggered) {
      unpairHolding = false;
      unpairTriggered = false;
      drawScreen();
    } else {
      releaseAction();
    }
  }

  if (unpairHolding && millis() - unpairHoldStartMs >= kUnpairHoldMs) {
    finishUnpairHold();
  } else if (unpairHolding && millis() - lastDrawMs >= 80) {
    drawScreen();
  }

  if (unpairNotice != UnpairNotice::None &&
      static_cast<int32_t>(millis() - unpairNoticeUntilMs) >= 0) {
    unpairNotice = UnpairNotice::None;
    drawScreen();
  }

  if (kHasPageButtons && M5.BtnA.wasPressed()) {
    page = Page::Tasks;
    drawScreen();
  } else if (kHasPageButtons && M5.BtnB.wasPressed()) {
    page = Page::Commands;
    drawScreen();
  } else if (kHasPageButtons && M5.BtnC.wasPressed()) {
    page = Page::Navigate;
    drawScreen();
  }

  CodexMicroState latest = codex.snapshot();
#if defined(CODEX_BOARD_TAB5)
  if (latest.connected != state.connected) {
    threadStateInitialized = false;
  }
  processThreadNotifications(latest);
  const bool headerChanged = latest.connected != state.connected ||
                             latest.secured != state.secured ||
                             latest.ready != state.ready ||
                             latest.diagnostic != state.diagnostic;
  std::array<bool, 6> changedTasks{};
  bool taskChanged = false;
  for (size_t i = 0; i < changedTasks.size(); ++i) {
    const ThreadLight& before = state.threads[i];
    const ThreadLight& after = latest.threads[i];
    changedTasks[i] = before.color != after.color ||
                      before.brightness != after.brightness ||
                      before.effect != after.effect ||
                      before.speed != after.speed;
    taskChanged = taskChanged || changedTasks[i];
  }
  state = latest;
  if (headerChanged) {
    requestDraw();
  } else if (page == Page::Control && taskChanged) {
    redrawTaskRegions(changedTasks);
  } else if (latest.dirty && page != Page::Control) {
    requestDraw();
  }
#else
  if (latest.dirty || latest.connected != state.connected ||
      latest.secured != state.secured || latest.ready != state.ready ||
      latest.diagnostic != state.diagnostic) {
    state = latest;
    requestDraw();
  } else {
    state = latest;
  }
#endif

  if (!state.standby && (page == Page::Tasks || page == Page::Control) &&
      millis() - lastDrawMs > 80) {
    std::array<bool, 6> animatedTasks{};
    bool animated = false;
    for (size_t i = 0; i < animatedTasks.size(); ++i) {
      const ThreadLight& light = state.threads[i];
      animatedTasks[i] = light.effect == "breath" ||
                         taskVisualState(light) == TaskVisualState::Error;
      animated = animated || animatedTasks[i];
    }
    if (animated) {
#if defined(CODEX_BOARD_TAB5)
      redrawTaskRegions(animatedTasks);
#else
      drawScreen();
#endif
    }
  }

#if defined(CODEX_BOARD_TAB5)
  if (drawPending && millis() - lastDrawMs >= 24) drawScreen();
  reportRenderTimings();
#endif

  updateBattery();
  delay(8);
}
