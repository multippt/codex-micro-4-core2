// SPDX-License-Identifier: MIT

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "BoardProfile.h"
#include "CodexMicroBle.h"
#include "StickS3Controller.h"

#if !defined(CODEX_BOARD_STICKS3)
#error "main_sticks3.cpp is only for the StickS3 build"
#endif

namespace {

using codex_micro::StickGesture;
using codex_micro::StickPage;

constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x10A2;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kMuted = 0x8410;
constexpr uint16_t kSelection = 0xFD20;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kBlue = 0x2E9F;
constexpr uint16_t kYellow = 0xFFE0;
constexpr int kHeaderHeight = 24;
constexpr int kFooterHeight = 28;
constexpr int kGap = 3;
constexpr uint32_t kNoticeMs = 3000;
constexpr char kPreferenceNamespace[] = "codex-micro";
constexpr char kSoundKey[] = "sound";
constexpr char kPendingUnpairKey[] = "pending-unpair";

enum class TaskVisualState : uint8_t { Idle, Active, Waiting, Complete, Error };
enum class Notice : uint8_t { None, Unpaired, Failed, Restarting };

const char* kAgentKeys[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05"};
const char* kCommandKeys[] = {"ACT06", "ACT07", "ACT08", "ACT09", "ACT10", "ACT12"};
constexpr uint8_t kCommandOrder[] = {0, 3, 1, 2, 4, 5};

CodexMicroBle codex;
CodexMicroState state;
M5Canvas canvas(&M5.Display);
Preferences preferences;
codex_micro::StickButtonController buttonB;
codex_micro::StickUiController ui;
bool soundEnabled = true;
bool speakerReady = false;
bool unpairBusy = false;
int batteryLevel = 100;
bool batteryCharging = false;
uint32_t lastBatteryMs = 0;
uint32_t lastDrawMs = 0;
uint32_t noticeUntilMs = 0;
Notice notice = Notice::None;

struct Rect { int x; int y; int w; int h; };

uint16_t rgb888To565(uint32_t color, float brightness = 1.0f) {
  const uint8_t r = static_cast<uint8_t>(((color >> 16) & 0xFF) * brightness);
  const uint8_t g = static_cast<uint8_t>(((color >> 8) & 0xFF) * brightness);
  const uint8_t b = static_cast<uint8_t>((color & 0xFF) * brightness);
  return canvas.color565(r, g, b);
}

TaskVisualState taskVisualState(const ThreadLight& light) {
  if (light.brightness <= 0.01f) return TaskVisualState::Idle;
  const uint8_t r = (light.color >> 16) & 0xFF;
  const uint8_t g = (light.color >> 8) & 0xFF;
  const uint8_t b = light.color & 0xFF;
  const uint8_t hi = std::max(r, std::max(g, b));
  const uint8_t lo = std::min(r, std::min(g, b));
  if (hi - lo < 40) return TaskVisualState::Idle;
  if (r > 180 && r > g * 3 / 2 && r > b * 6 / 5) return TaskVisualState::Error;
  if (r > 160 && g > 100 && b < 120) return TaskVisualState::Waiting;
  if (g > r * 6 / 5 && g > b * 6 / 5) return TaskVisualState::Complete;
  if (b > r * 6 / 5 && b > g * 6 / 5) return TaskVisualState::Active;
  return TaskVisualState::Idle;
}

uint16_t taskColor(const ThreadLight& light) {
  if (light.brightness <= 0.01f) return kMuted;
  float pulse = 1.0f;
  if (light.effect == "breath") {
    pulse = 0.55f + 0.45f * (std::sin(millis() * 0.006f) * 0.5f + 0.5f);
  }
  return rgb888To565(light.color, light.brightness * pulse);
}

void drawText(const char* text, int x, int y, int size = 1,
              uint16_t color = kText) {
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

void drawBluetooth(int x, int y, uint16_t color) {
  canvas.drawLine(x, y - 7, x, y + 7, color);
  canvas.drawLine(x, y - 7, x + 5, y - 2, color);
  canvas.drawLine(x + 5, y - 2, x - 3, y + 4, color);
  canvas.drawLine(x - 3, y - 4, x + 5, y + 2, color);
  canvas.drawLine(x + 5, y + 2, x, y + 7, color);
}

void drawBattery(int x, int y) {
  constexpr int w = 20;
  constexpr int h = 10;
  canvas.drawRect(x, y - h / 2, w, h, kText);
  canvas.fillRect(x + w, y - 2, 2, 4, kText);
  const int level = constrain(batteryLevel, 0, 100);
  const int fill = (w - 4) * level / 100;
  canvas.fillRect(x + 2, y - h / 2 + 2, fill, h - 4,
                  level < 20 ? kRed : kGreen);
  if (batteryCharging) {
    canvas.drawLine(x + 10, y - 4, x + 7, y + 1, kYellow);
    canvas.drawLine(x + 7, y + 1, x + 11, y, kYellow);
    canvas.drawLine(x + 11, y, x + 8, y + 5, kYellow);
  }
}

const char* pageTitle() {
  if (ui.confirmingUnpair()) return "UNPAIR?";
  switch (ui.page()) {
    case StickPage::Agents: return "AGENTS";
    case StickPage::Commands: return "COMMANDS";
    case StickPage::Navigate: return "NAV";
    case StickPage::Config: return "CONFIG";
    default: return "CODEX";
  }
}

void drawHeader() {
  canvas.fillRect(0, 0, canvas.width(), kHeaderHeight, kBackground);
  canvas.setTextDatum(middle_left);
  canvas.setTextSize(1);
  canvas.setTextColor(kText);
  canvas.drawString(pageTitle(), 4, kHeaderHeight / 2);
  drawBluetooth(canvas.width() - 35, kHeaderHeight / 2,
                state.connected ? kBlue : kMuted);
  drawBattery(canvas.width() - 25, kHeaderHeight / 2);
}

void drawTick(const Rect& r, uint16_t color) {
  const int x1 = r.x + r.w * 25 / 100;
  const int y1 = r.y + r.h * 52 / 100;
  const int x2 = r.x + r.w * 44 / 100;
  const int y2 = r.y + r.h * 70 / 100;
  const int x3 = r.x + r.w * 76 / 100;
  const int y3 = r.y + r.h * 30 / 100;
  for (int i = -2; i <= 2; ++i) {
    canvas.drawLine(x1, y1 + i, x2, y2 + i, color);
    canvas.drawLine(x2, y2 + i, x3, y3 + i, color);
  }
}

void drawCross(const Rect& r, uint16_t color) {
  const int insetX = r.w * 28 / 100;
  const int insetY = r.h * 28 / 100;
  for (int i = -2; i <= 2; ++i) {
    canvas.drawLine(r.x + insetX + i, r.y + insetY,
                    r.x + r.w - insetX + i, r.y + r.h - insetY, color);
    canvas.drawLine(r.x + r.w - insetX + i, r.y + insetY,
                    r.x + insetX + i, r.y + r.h - insetY, color);
  }
}

void drawArrow(const Rect& r, int dx, int dy, uint16_t color) {
  const int cx = r.x + r.w / 2;
  const int cy = r.y + r.h / 2;
  const int length = std::min(r.w, r.h) / 3;
  const int ex = cx + dx * length;
  const int ey = cy + dy * length;
  const int sx = cx - dx * length;
  const int sy = cy - dy * length;
  for (int i = -1; i <= 1; ++i) {
    canvas.drawLine(sx + (dy ? i : 0), sy + (dx ? i : 0),
                    ex + (dy ? i : 0), ey + (dx ? i : 0), color);
  }
  canvas.drawLine(ex, ey, ex - dx * 7 + dy * 6, ey - dy * 7 + dx * 6, color);
  canvas.drawLine(ex, ey, ex - dx * 7 - dy * 6, ey - dy * 7 - dx * 6, color);
}

void drawTile(const Rect& r, uint8_t index, const char* label = nullptr,
              uint16_t border = kMuted, uint16_t fill = kPanel) {
  canvas.fillRoundRect(r.x, r.y, r.w, r.h, 4, fill);
  canvas.drawRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 3, border);
  if (ui.selection() == index) {
    canvas.drawRoundRect(r.x, r.y, r.w, r.h, 4, kSelection);
    canvas.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, kSelection);
  }
  if (label != nullptr) {
    drawText(label, r.x + r.w / 2, r.y + r.h / 2,
             strlen(label) <= 2 && r.h > 45 ? 3 : (r.h > 38 ? 2 : 1));
  }
}

Rect gridRect(int index, int top, int bottom, int columns, int rows) {
  const int width = canvas.width() - 2 * kGap;
  const int height = bottom - top;
  const int cellW = (width - (columns - 1) * kGap) / columns;
  const int cellH = (height - (rows - 1) * kGap) / rows;
  const int row = index / columns;
  const int col = index % columns;
  return {kGap + col * (cellW + kGap), top + row * (cellH + kGap), cellW, cellH};
}

void drawFooter(uint8_t previousIndex, uint8_t nextIndex) {
  const int y = canvas.height() - kFooterHeight;
  const Rect left{3, y, (canvas.width() - 9) / 2, kFooterHeight - 3};
  const Rect right{left.x + left.w + 3, y, canvas.width() - left.w - 9,
                   kFooterHeight - 3};
  drawTile(left, previousIndex);
  drawTile(right, nextIndex);
  drawArrow(left, -1, 0, kText);
  drawArrow(right, 1, 0, kText);
}

void drawAgents() {
  const int bottom = canvas.height() - kFooterHeight - 3;
  for (int i = 0; i < 6; ++i) {
    const Rect r = gridRect(i, kHeaderHeight + 3, bottom, 2, 3);
    char number[2] = {static_cast<char>('1' + i), '\0'};
    const uint16_t status = taskColor(state.threads[i]);
    drawTile(r, i, number, status, kPanel);
  }
  drawFooter(6, 7);
}

void drawCommands() {
  const int bottom = canvas.height() - kFooterHeight - 3;
  const char* labels[] = {"FAST", "FORK", nullptr, nullptr, "MIC", "SEND"};
  for (int i = 0; i < 6; ++i) {
    const Rect r = gridRect(i, kHeaderHeight + 3, bottom, 2, 3);
    drawTile(r, i, labels[i]);
    if (i == 2) drawTick(r, kGreen);
    if (i == 3) drawCross(r, kRed);
  }
  drawFooter(6, 7);
}

void drawNavigate() {
  const int top = kHeaderHeight + 3;
  const int footerTop = canvas.height() - kFooterHeight - 3;
  const int dpadBottom = top + 82;
  for (int i = 0; i < 4; ++i) {
    const Rect r = gridRect(i, top, dpadBottom, 2, 2);
    drawTile(r, i);
    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    drawArrow(r, dx[i], dy[i], kText);
  }
  const int midTop = dpadBottom + 3;
  const int midBottom = midTop + 36;
  for (int i = 0; i < 2; ++i) {
    const Rect r = gridRect(i, midTop, midBottom, 2, 1);
    drawTile(r, static_cast<uint8_t>(4 + i), i == 0 ? "CCW" : "CW");
  }
  const Rect dial{3, midBottom + 3, canvas.width() - 6,
                  footerTop - midBottom - 3};
  drawTile(dial, 6, "DIAL");
  drawFooter(7, 8);
}

void drawConfig() {
  const int bottom = canvas.height() - kFooterHeight - 3;
  const Rect unpair = gridRect(0, kHeaderHeight + 3, bottom, 1, 2);
  const Rect mute = gridRect(1, kHeaderHeight + 3, bottom, 1, 2);
  drawTile(unpair, 0, "UNPAIR");
  drawTile(mute, 1, soundEnabled ? "MUTE" : "UNMUTE");
  drawFooter(2, 3);
}

void drawUnpairConfirmation() {
  const int top = 62;
  const int height = 112;
  const Rect cancel{5, top, (canvas.width() - 13) / 2, height};
  const Rect confirm{cancel.x + cancel.w + 3, top,
                     canvas.width() - cancel.w - 13, height};
  drawTile(cancel, 0, nullptr, kRed, 0x3000);
  drawTile(confirm, 1, nullptr, kGreen, 0x0180);
  drawCross(cancel, kText);
  drawTick(confirm, kText);
  drawText("CONFIRM", canvas.width() / 2, 198, 1, kMuted);
}

void drawNotice() {
  if (notice == Notice::None) return;
  canvas.fillRoundRect(7, 82, canvas.width() - 14, 70, 6, kBackground);
  canvas.drawRoundRect(7, 82, canvas.width() - 14, 70, 6,
                       notice == Notice::Failed ? kRed : kGreen);
  const char* text = notice == Notice::Unpaired ? "UNPAIRED"
                     : notice == Notice::Restarting ? "RESTARTING"
                                                    : "FAILED";
  drawText(text, canvas.width() / 2, 117, 1);
}

void drawScreen() {
  canvas.fillScreen(kBackground);
  drawHeader();
  if (ui.confirmingUnpair()) {
    drawUnpairConfirmation();
  } else {
    switch (ui.page()) {
      case StickPage::Agents: drawAgents(); break;
      case StickPage::Commands: drawCommands(); break;
      case StickPage::Navigate: drawNavigate(); break;
      case StickPage::Config: drawConfig(); break;
      default: break;
    }
  }
  drawNotice();
  canvas.pushSprite(0, 0);
  lastDrawMs = millis();
}

void logSelectorChange(const char* source, StickPage previousPage,
                       uint8_t previousSelection) {
  if (ui.page() == previousPage && ui.selection() == previousSelection) return;
  Serial.printf("StickS3 selector source=%s page=%u->%u selection=%u->%u\n",
                source, static_cast<unsigned>(previousPage),
                static_cast<unsigned>(ui.page()), previousSelection,
                ui.selection());
}

void changePage(int8_t delta, codex_micro::StickPageSelection destination) {
  const StickPage previousPage = ui.page();
  const uint8_t previousSelection = ui.selection();
  ui.changePage(delta, destination);
  logSelectorChange("page-arrow", previousPage, previousSelection);
  drawScreen();
}

void handleGesture(StickGesture gesture) {
  const StickPage previousPage = ui.page();
  const uint8_t previousSelection = ui.selection();
  if (gesture == StickGesture::Next) ui.move(1);
  else if (gesture == StickGesture::Previous) ui.move(-1);
  else if (gesture == StickGesture::NextPage && !ui.confirmingUnpair()) {
    ui.changePage(1);
  }
  if (gesture != StickGesture::None) {
    logSelectorChange("side-button", previousPage, previousSelection);
    drawScreen();
  }
}

void tapKey(const char* key, int8_t agent = -1) {
  codex.sendKey(key, 1, agent);
  delay(8);
  codex.sendKey(key, 0, agent);
}

void toggleSound() {
  soundEnabled = !soundEnabled;
  preferences.begin(kPreferenceNamespace, false);
  preferences.putBool(kSoundKey, soundEnabled);
  preferences.end();
  Serial.printf("StickS3 notifications %s\n", soundEnabled ? "enabled" : "muted");
}

void finishUnpair() {
  if (unpairBusy) return;
  unpairBusy = true;
  drawScreen();
  const BondClearResult result = codex.clearBonds();
  if (result == BondClearResult::RestartRequired) {
    preferences.begin(kPreferenceNamespace, false);
    const bool saved = preferences.putBool(kPendingUnpairKey, true) == sizeof(bool);
    preferences.end();
    if (saved) {
      notice = Notice::Restarting;
      drawScreen();
      delay(750);
      ESP.restart();
    }
  }
  notice = result == BondClearResult::Success ? Notice::Unpaired : Notice::Failed;
  noticeUntilMs = millis() + kNoticeMs;
  unpairBusy = false;
  ui.cancelUnpairConfirmation();
  drawScreen();
}

void activateSelection() {
  if (unpairBusy) return;
  if (ui.confirmingUnpair()) {
    if (ui.confirmsUnpair()) finishUnpair();
    else {
      ui.cancelUnpairConfirmation();
      drawScreen();
    }
    return;
  }

  const uint8_t selected = ui.selection();
  if (selected == ui.itemCount() - 2) {
    return changePage(-1, codex_micro::StickPageSelection::PreviousArrow);
  }
  if (selected == ui.itemCount() - 1) {
    return changePage(1, codex_micro::StickPageSelection::NextArrow);
  }

  switch (ui.page()) {
    case StickPage::Agents:
      if (selected < 6) {
        Serial.printf("StickS3 activate agent=%u\n", selected);
        tapKey(kAgentKeys[selected], selected);
      }
      break;
    case StickPage::Commands:
      if (selected < 6) tapKey(kCommandKeys[kCommandOrder[selected]]);
      break;
    case StickPage::Navigate:
      if (selected < 4) {
        const float angles[] = {0.75f, 0.25f, 0.5f, 0.0f};
        codex.sendJoystick(angles[selected], 1.0f);
        delay(8);
        codex.sendJoystick(angles[selected], 0.0f);
      } else if (selected == 4) codex.sendKey("ENC_CC", 2);
      else if (selected == 5) codex.sendKey("ENC_CW", 2);
      else if (selected == 6) tapKey("ENC");
      break;
    case StickPage::Config:
      if (selected == 0) ui.beginUnpairConfirmation();
      else if (selected == 1) toggleSound();
      drawScreen();
      break;
    default: break;
  }
}

void updateRecentAgent(const CodexMicroState& latest) {
  int newest = -1;
  int newestUrgent = -1;
  uint32_t newestOrder = 0;
  uint32_t newestUrgentOrder = 0;
  bool attention = false;
  for (int i = 0; i < 6; ++i) {
    if (latest.threadUpdateOrder[i] == state.threadUpdateOrder[i]) continue;
    const uint32_t order = latest.threadUpdateOrder[i];
    const auto visual = taskVisualState(latest.threads[i]);
    if (order >= newestOrder) {
      newestOrder = order;
      newest = i;
    }
    if (codex_micro::stickAgentUrgent(static_cast<uint8_t>(visual)) &&
        order >= newestUrgentOrder) {
      newestUrgentOrder = order;
      newestUrgent = i;
      attention = true;
    }
  }
  const int chosen = newestUrgent >= 0 ? newestUrgent : newest;
  if (chosen >= 0) {
    const StickPage previousPage = ui.page();
    const uint8_t previousSelection = ui.selection();
    ui.noteAgent(static_cast<uint8_t>(chosen));
    logSelectorChange("host-activity", previousPage, previousSelection);
  }
  if (attention && soundEnabled && speakerReady) M5.Speaker.tone(1000, 90);
}

void updateBattery() {
  if (lastBatteryMs != 0 && millis() - lastBatteryMs < 30000) return;
  lastBatteryMs = millis();
  const int level = M5.Power.getBatteryLevel();
  batteryLevel = level < 0 ? 100 : level;
  batteryCharging = M5.Power.isCharging();
  codex.setBattery(static_cast<uint8_t>(batteryLevel), batteryCharging);
}

bool hasAnimation() {
  for (const ThreadLight& light : state.threads) {
    if (light.effect == "breath") return true;
  }
  return false;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(kBootMessage);

  preferences.begin(kPreferenceNamespace, false);
  soundEnabled = preferences.getBool(kSoundKey, true);
  const bool pendingUnpair = preferences.getBool(kPendingUnpairKey, false);
  if (pendingUnpair) preferences.remove(kPendingUnpairKey);
  preferences.end();

  auto config = M5.config();
  config.clear_display = true;
  M5.begin(config);
  M5.Display.setRotation(kDisplayRotation);
  M5.Display.setBrightness(kDisplayBrightness);
  M5.Display.setTextWrap(false);
  speakerReady = M5.Speaker.begin();
  if (speakerReady) M5.Speaker.setVolume(96);

  canvas.setColorDepth(16);
  if (canvas.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.drawString("Canvas failed", 5, 100);
    while (true) delay(1000);
  }
  canvas.setTextWrap(false);
  drawScreen();

  codex.begin();
  state = codex.snapshot();
  if (pendingUnpair) {
    const BondClearResult result = codex.clearBonds();
    notice = result == BondClearResult::Success ? Notice::Unpaired : Notice::Failed;
    noticeUntilMs = millis() + kNoticeMs;
  }
  updateBattery();
  drawScreen();
  Serial.println("CODEX_MICRO_READY");
}

void loop() {
  M5.update();
  codex.maintain();
  const uint32_t now = millis();

  // Consume host state first so physical input remains the final selection
  // authority when both arrive during the same loop iteration.
  CodexMicroState latest = codex.snapshot();
  const bool stateChanged = latest.dirty || latest.connected != state.connected ||
                            latest.threadRevision != state.threadRevision;
  updateRecentAgent(latest);
  state = latest;
  if (stateChanged) drawScreen();

  if (M5.BtnB.wasPressed()) buttonB.pressed(now);
  handleGesture(buttonB.update(now));
  if (M5.BtnB.wasReleased()) handleGesture(buttonB.released(now));
  if (M5.BtnA.wasReleased()) activateSelection();

  if (notice != Notice::None && static_cast<int32_t>(now - noticeUntilMs) >= 0) {
    notice = Notice::None;
    drawScreen();
  }
  if (ui.page() == StickPage::Agents && hasAnimation() && now - lastDrawMs >= 80) {
    drawScreen();
  }
  updateBattery();
  delay(8);
}
