// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_config.h"

namespace config {

Option<std::string> GdxLobbyServer("server", "lbs.gdxsv.net", "gdxsv");
Option<std::string> GdxLoginKey("loginkey", "", "gdxsv");
Option<int> GdxLanguage("language", -1, "gdxsv");
Option<bool> GdxUseTexturePack("UseTexturePack", false, "gdxsv");
Option<bool> GdxFontRaster("FontRaster", false, "gdxsv");
Option<std::string, false> GdxFontRasterFile("FontRasterFile", "", "gdxsv");
Option<int> GdxWidescreenHudLayout("WidescreenHudLayout", 1, "gdxsv");
OptionString GdxTexturePackChannel("TexturePackChannel", "", "gdxsv");
Option<int> GdxLocalPort("LocalPort", 0, "gdxsv");
Option<int> GdxMinDelay("MinDelay", 2, "gdxsv");
Option<bool> GdxSaveReplay("SaveReplay", true, "gdxsv");
Option<bool> GdxUploadReplay("UploadReplay", true, "gdxsv");
Option<std::string, false> GdxReplayPath("ReplayPath", "", "gdxsv");
Option<bool> GdxSkipRenderingHack("SkipRenderingHack", true, "gdxsv");
Option<bool> GdxSlowIdleLoopHack("SlowIdleLoopHack", true, "gdxsv");

Option<bool> GdxReplayHideName("ReplayHideName", false, "gdxsv");
Option<bool> GdxReplayShowAllyHP("ReplayShowAllyHP", true, "gdxsv");
Option<bool> GdxReplayKeyDisplay("ReplayKeyDisplay", true, "gdxsv");
Option<bool> GdxReplaySkipMsSelection("ReplaySkipMsSelection", true, "gdxsv");
Option<bool> GdxProjectileView("ProjectileView", false, "gdxsv");
Option<bool> GdxLiveAutoNext("LiveAutoNext", false, "gdxsv");

}  // namespace config
