// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The replay god's eye view (Disc 2): a camera above every player, computed here and shown by the guest code of
// gdxsv_patch/src/camera.c.
#pragma once
#include <string>

// The replay's point of view: the POV player's own camera, or the god's eye view of all players. The god's eye view still runs on
// the POV player's console.
enum class GdxsvPovMode { Player, GodsEye };

void gdxsv_camera_set_pov_mode(GdxsvPovMode mode);
// "player" or "godseye", the values of gdxsv:ReplayPOVMode; false, and no change, for any other.
bool gdxsv_camera_set_pov_mode_name(const std::string &name);
GdxsvPovMode gdxsv_camera_pov_mode();
// A mode's name as the POV menu shows it, untranslated ("God's Eye View"); empty for Player.
const char *gdxsv_camera_pov_mode_label(GdxsvPovMode mode);

// Emulator thread, between SH4 runs: installs the guest code on first use and keeps its hooks in step with the mode.
void gdxsv_camera_next_frame(int disk, bool replaying);
void gdxsv_camera_reset();
// A state was restored: it can predate the guest code while the code cache still holds blocks compiled from it.
void gdxsv_camera_state_loaded();
// Render thread, inside the ImGui frame, in battle: the nameplates, trails and lock pointers of the god's eye view.
void gdxsv_camera_render_overlay();
