// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#include <Arduino.h>
#include <M5Unified.h>

#include <cmath>

#include "BoardProfile.h"
#include "CodexMicroBle.h"

namespace {

enum class Page : uint8_t { Tasks, Commands, Navigate };

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

const char* kAgentKeys[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05"};
const char* kCommandKeys[] = {"ACT06", "ACT07", "ACT08", "ACT09", "ACT10", "ACT12"};
const char* kCommandLabels[] = {"FAST", "APPROVE", "DECLINE", "FORK", "MIC", "SEND"};

CodexMicroBle codex;
CodexMicroState state;
M5Canvas canvas(&M5.Display);
Page page = Page::Tasks;
TouchAction activeAction;
bool touchActive = false;
uint32_t lastDrawMs = 0;
uint32_t lastBatteryMs = 0;
Layout layout{};

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

void drawCentered(const char* text, int x, int y, int font = 1, uint16_t color = kText) {
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(font);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

void drawHeader() {
  canvas.fillRect(0, 0, layout.width, layout.headerHeight, kBackground);
  canvas.setTextDatum(middle_left);
  canvas.setTextSize(2 * layout.textScale);
  canvas.setTextColor(kText);
  canvas.drawString("CODEX MICRO", layout.margin, layout.headerHeight / 2);

  const uint16_t dot = state.connected ? 0x07E0 : 0xF800;
  const int dotX = layout.width - layout.margin - 95 * layout.textScale;
  canvas.fillCircle(dotX, layout.headerHeight / 2, 4 * layout.textScale, dot);
  canvas.setTextDatum(middle_right);
  canvas.setTextSize(layout.textScale);
  canvas.setTextColor(kMuted);
  canvas.drawString(state.connected ? "LIVE" : "PAIR", layout.width - layout.margin,
                    layout.headerHeight / 2);
}

void drawTabs() {
  const char* labels[] = {"TASKS", "COMMANDS", "NAVIGATE"};
  for (int i = 0; i < 3; ++i) {
    const int x = i * layout.width / 3;
    const int width = (i + 1) * layout.width / 3 - x;
    const bool selected = static_cast<int>(page) == i;
    canvas.fillRect(x, layout.contentBottom, width, layout.tabHeight,
                    selected ? kAccent : kPanel);
    drawCentered(labels[i], x + width / 2, layout.contentBottom + layout.tabHeight / 2,
                 layout.textScale,
                 selected ? kText : kMuted);
  }
}

void drawButton(int x, int y, int width, int height, const char* label, uint16_t border,
                bool pressed = false, const char* sublabel = nullptr) {
  canvas.fillRoundRect(x, y, width, height, 6, pressed ? kPanelPressed : kPanel);
  canvas.drawRoundRect(x, y, width, height, 6, border);
  drawCentered(label, x + width / 2, y + height / 2 - (sublabel ? 7 : 0),
               (strlen(label) > 7 ? 1 : 2) * layout.textScale, kText);
  if (sublabel != nullptr) {
    drawCentered(sublabel, x + width / 2, y + height / 2 + 13 * layout.textScale,
                 layout.textScale, kMuted);
  }
}

void drawTasks() {
  const int buttonWidth = (layout.width - 2 * layout.margin - 2 * layout.gap) / 3;
  const int buttonHeight = (layout.contentBottom - layout.contentTop -
                            2 * layout.margin - layout.gap) / 2;
  for (int i = 0; i < 6; ++i) {
    const int row = i / 3;
    const int col = i % 3;
    const int x = layout.margin + col * (buttonWidth + layout.gap);
    const int y = layout.contentTop + layout.margin + row * (buttonHeight + layout.gap);
    const ThreadLight& light = state.threads[i];
    float pulse = 1.0f;
    if (light.effect == "breath") {
      pulse = 0.55f + 0.45f * (std::sin(millis() * 0.006f) * 0.5f + 0.5f);
    }
    const uint16_t color = light.brightness <= 0.01f
                               ? 0x4208
                               : rgb888To565(light.color, light.brightness * pulse);
    char title[12];
    snprintf(title, sizeof(title), "AGENT %d", i + 1);
    const char* status = light.brightness <= 0.01f ? "UNASSIGNED" : light.effect.c_str();
    const bool pressed = touchActive && activeAction.agent == i;
    drawButton(x, y, buttonWidth, buttonHeight, title, color, pressed, status);
    canvas.fillCircle(x + buttonWidth - 12 * layout.textScale,
                      y + 12 * layout.textScale, 4 * layout.textScale, color);
  }
}

void drawCommands() {
  const int buttonWidth = (layout.width - 2 * layout.margin - 2 * layout.gap) / 3;
  const int buttonHeight = (layout.contentBottom - layout.contentTop -
                            2 * layout.margin - layout.gap) / 2;
  for (int i = 0; i < 6; ++i) {
    const int row = i / 3;
    const int col = i % 3;
    const int x = layout.margin + col * (buttonWidth + layout.gap);
    const int y = layout.contentTop + layout.margin + row * (buttonHeight + layout.gap);
    const bool pressed = touchActive && activeAction.key == kCommandKeys[i];
    const char* hint = i == 4 ? "HOLD / 2X" : "TAP";
    drawButton(x, y, buttonWidth, buttonHeight, kCommandLabels[i],
               i == 1 ? 0x07E0 : kAccent, pressed, hint);
  }
}

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

void drawScreen() {
  canvas.fillScreen(kBackground);
  drawHeader();
  switch (page) {
    case Page::Tasks:
      drawTasks();
      break;
    case Page::Commands:
      drawCommands();
      break;
    case Page::Navigate:
      drawNavigate();
      break;
  }
  drawTabs();
  canvas.pushSprite(0, 0);
  lastDrawMs = millis();
}

bool inRect(int x, int y, int left, int top, int width, int height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

TouchAction actionAt(int x, int y) {
  if (y >= layout.contentBottom) {
    page = static_cast<Page>(min(2, x * 3 / layout.width));
    drawScreen();
    return {};
  }

  if (page == Page::Tasks) {
    const int bw = (layout.width - 2 * layout.margin - 2 * layout.gap) / 3;
    const int bh = (layout.contentBottom - layout.contentTop - 2 * layout.margin - layout.gap) / 2;
    if (x >= layout.margin && y >= layout.contentTop + layout.margin) {
      const int col = (x - layout.margin) / (bw + layout.gap);
      const int row = (y - layout.contentTop - layout.margin) / (bh + layout.gap);
      if (col < 3 && row < 2 && inRect(x, y, layout.margin + col * (bw + layout.gap),
                                      layout.contentTop + layout.margin + row * (bh + layout.gap), bw, bh)) {
        const int index = row * 3 + col;
        return {kAgentKeys[index], static_cast<int8_t>(index), false, false, 0.0f};
      }
    }
  } else if (page == Page::Commands) {
    const int bw = (layout.width - 2 * layout.margin - 2 * layout.gap) / 3;
    const int bh = (layout.contentBottom - layout.contentTop - 2 * layout.margin - layout.gap) / 2;
    if (x >= layout.margin && y >= layout.contentTop + layout.margin) {
      const int col = (x - layout.margin) / (bw + layout.gap);
      const int row = (y - layout.contentTop - layout.margin) / (bh + layout.gap);
      if (col < 3 && row < 2 && inRect(x, y, layout.margin + col * (bw + layout.gap),
                                      layout.contentTop + layout.margin + row * (bh + layout.gap), bw, bh)) {
        const int index = row * 3 + col;
        return {kCommandKeys[index], -1, false, false, 0.0f};
      }
    }
  } else {
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
  drawScreen();
}

void releaseAction() {
  if (!touchActive) return;
  if (activeAction.joystick) {
    codex.sendJoystick(activeAction.angle, 0.0f);
  } else if (!activeAction.encoderStep && activeAction.key != nullptr) {
    codex.sendKey(activeAction.key, 0, activeAction.agent);
  }
  touchActive = false;
  activeAction = {};
  drawScreen();
}

void updateBattery() {
  if (millis() - lastBatteryMs < 30000 && lastBatteryMs != 0) return;
  lastBatteryMs = millis();
  const int level = M5.Power.getBatteryLevel();
  const bool charging = M5.Power.isCharging();
  codex.setBattery(level < 0 ? 100 : static_cast<uint8_t>(level), charging);
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
  updateBattery();
  drawScreen();
  Serial.println("CODEX_MICRO_READY");
}

void loop() {
  M5.update();
  const auto touch = M5.Touch.getDetail();
  if (touch.wasPressed()) {
    pressAction(actionAt(touch.x, touch.y));
  }
  if (touch.wasReleased()) {
    releaseAction();
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
  if (latest.dirty || latest.connected != state.connected) {
    state = latest;
    drawScreen();
  } else {
    state = latest;
  }

  if (page == Page::Tasks && millis() - lastDrawMs > 80) {
    bool animated = false;
    for (const ThreadLight& light : state.threads) {
      animated = animated || light.effect == "breath";
    }
    if (animated) drawScreen();
  }

  updateBattery();
  delay(8);
}
