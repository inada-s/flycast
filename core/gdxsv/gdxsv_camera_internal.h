// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// What the files of the replay god's eye view share. Only they include this header.
#pragma once
#include <atomic>
#include <string>

#include "gdxsv_camera.h"
#include "types.h"

// Degrees the god's eye view leans back from straight down.
constexpr float kGdxsvCameraAngleMax = 70.f;
constexpr float kGdxsvCameraAngleDefault = 45.f;
// The closest the framing brings the eye to the players, in world units.
constexpr float kGdxsvCameraClosestMin = 150.f;
constexpr float kGdxsvCameraClosestMax = 1500.f;
// The viewer's zoom: a factor on the distance that frames the followed players.
constexpr float kGdxsvCameraZoomMin = 0.25f;
constexpr float kGdxsvCameraZoomMax = 4.f;
constexpr u32 kGdxsvCameraEveryone = 0xf;
constexpr float kGdxsvCameraTrailSecondsMax = 20.f;
// World units: a standing Gundam or Gelgoog, from its feet up.
constexpr float kGdxsvCameraMsHeight = 18.f;

// The game's four players (T13306M disc 2), and what the god's eye view reads of their actors.
constexpr u32 kGdxsvCameraPlayerWork = 0x0c3d1cd4;
constexpr u32 kGdxsvCameraPlayerStride = 0x2000;
constexpr u32 kGdxsvCameraActorState = 4;		   // byte: 1 in play, 2 wrecked, 3 gone for a frame
constexpr u32 kGdxsvCameraActorPhase = 5;		   // byte: 1 while it arrives (drops in, then waits), 3 in control, 4 after the round
constexpr u32 kGdxsvCameraActorPosition = 0x20;	   // x, y, z of its feet
constexpr u32 kGdxsvCameraActorMoveFlags = 0x23c;  // bit 3 set while off the ground or a surface in space
constexpr u16 kGdxsvCameraMoveAirborne = 0x0008;

// What the viewer picked. Written by the UI, read by the emulator and render threads.
struct GdxsvCameraSettings {
	std::atomic<GdxsvPovMode> pov_mode{GdxsvPovMode::Player};
	std::atomic<float> yaw{0.f};  // degrees from the briefing map's orientation, +X up
	std::atomic<float> angle{kGdxsvCameraAngleDefault};
	std::atomic<float> closest{kGdxsvCameraClosestMin};
	std::atomic<float> zoom{1.f};
	std::atomic<u32> focus{kGdxsvCameraEveryone};  // a bit per player the framing follows
	std::atomic<s64> hand_until{0};				   // steady clock, ns: a hand turns or tilts the view until then
	std::atomic<bool> nameplates{true};
	std::atomic<bool> boost{true};	// a boost gauge under each nameplate
	std::atomic<bool> team_colors{true};
	std::atomic<bool> ms_image{false};
	std::atomic<bool> fade{true};  // a nameplate fades while it overlaps another nameplate or MS
	std::atomic<bool> trail{true};
	std::atomic<float> trail_seconds{6.f};
	std::atomic<bool> dpad{true};  // the d-pad turns and zooms the view, not the replay
};

extern GdxsvCameraSettings gdxsv_camera_settings;

struct GdxsvCameraVec3 {
	float x = 0.f, y = 0.f, z = 0.f;
};

inline float gdxsv_camera_dot(const GdxsvCameraVec3 &a, const GdxsvCameraVec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// One frame of the god's eye view, in world units.
struct GdxsvCameraFrame {
	struct Player {
		bool on = false;	  // in the battle, its position known
		bool landed = false;  // not dropping in after a respawn
		GdxsvCameraVec3 at;	  // its feet
	} players[4];
	GdxsvCameraVec3 eye, right, up, forward;
	float aspect = 4.f / 3.f;  // the game's projection's
};

float gdxsv_camera_read_float(u32 addr);

// A float of a player's own camera record. While the game renders, the guest code has the god's eye view in the POV player's
// record and keeps that player's own words.
float gdxsv_camera_player_camera(int player, u32 offset);
// Emulator thread: what the overlay shows of this frame of the god's eye view, or nothing.
void gdxsv_camera_overlay_update(const GdxsvCameraFrame &frame);
void gdxsv_camera_overlay_clear();
// What the CAMERA panel shows of the battle: how far the TIME block has slid, in the game's pixels, and each player's
// force and name, empty past the replay's players.
struct GdxsvCameraBattle {
	float timer_slide = 0.f;
	int team[4] = {};
	std::string name[4];
};
// Render thread: false while the battle view is not up.
bool gdxsv_camera_overlay_battle(GdxsvCameraBattle &battle);
// A force's color as the overlay draws it: 0 Federation, 1 Zeon.
u32 gdxsv_camera_team_color(int team);
void gdxsv_camera_panel_reset();
// The aspect of the game's picture in a window of this aspect.
float gdxsv_camera_picture_aspect(float window_aspect);
// Called every frame a hand turns or tilts the view: its distance stays put until a little after the last call.
void gdxsv_camera_hold_distance();
// Writes the settings the viewer keeps to emu.cfg.
void gdxsv_camera_save_settings();
