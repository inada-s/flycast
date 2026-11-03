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
// Inside the replay's Tab menu, after its "POV" label: a button for each mode, the one in use lit, and a gear that opens
// the god's eye view's settings beside the menu. cell and height: one round button's size, the gear's. True when the
// mode changed, which a paused replay shows only after a frame.
bool gdxsv_camera_render_pov_row(int pov_player, float cell, float height);
// A frame without the Tab menu: closes the POV row's popup, so that it does not come back with the menu.
void gdxsv_camera_pov_row_hidden();
