// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gdxsv options ([gdxsv] section of emu.cfg). Kept in config:: under their old names.
#include "cfg/option.h"

namespace config {

extern Option<std::string> GdxLobbyServer;
extern Option<std::string> GdxLoginKey;
extern Option<int> GdxLanguage;
// Texture pack layers at 1, 2 or 4 (gdxsv_texture_pack_resolution)
extern Option<int> GdxTextureResolution2D;
extern Option<int> GdxTextureResolution3D;
extern Option<bool> GdxFontRaster;
extern Option<int> GdxWidescreenHudLayout;	// 0: stock 4:3, 1: up to 16:9, 2: viewport edges
extern OptionString GdxTexturePackChannel;
extern Option<int> GdxLocalPort;
extern Option<int> GdxMinDelay;
// Frames of the one-way latency left to rollback rather than to input delay; updated after each battle
extern Option<float> GdxRollbackBudget;
extern Option<bool> GdxSaveReplay;
extern Option<bool> GdxUploadReplay;
extern Option<std::string, false> GdxReplayPath;  // empty: data/replays
extern Option<bool> GdxSkipRenderingHack;
extern Option<bool> GdxSlowIdleLoopHack;
extern Option<int> GdxMaxFrameLatency;  // DX11: frames DXGI may queue for display; 0: driver default
extern Option<bool> GdxReplayHideName;
extern Option<bool> GdxReplayShowAllyHP;
extern Option<bool> GdxReplayKeyDisplay;
extern Option<bool> GdxReplaySkipMsSelection;
extern Option<bool> GdxLiveAutoNext;

}  // namespace config
