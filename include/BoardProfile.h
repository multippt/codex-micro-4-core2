// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#pragma once

#if defined(CODEX_BOARD_TAB5)
constexpr char kBoardName[] = "M5Stack Tab5";
constexpr char kFirmwareVersion[] = "0.2.0-tab5";
constexpr char kBootMessage[] = "Codex Micro Tab5 boot";
constexpr bool kHasPageButtons = false;
constexpr uint8_t kDisplayRotation = 1;
constexpr uint8_t kDisplayBrightness = 160;
#else
constexpr char kBoardName[] = "M5Stack Core2";
constexpr char kFirmwareVersion[] = "0.2.0-core2";
constexpr char kBootMessage[] = "Codex Micro Core2 boot";
constexpr bool kHasPageButtons = true;
constexpr uint8_t kDisplayRotation = 1;
constexpr uint8_t kDisplayBrightness = 120;
#endif
