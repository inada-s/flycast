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

// The game's four players, and what the god's eye view reads of their actors the same on both discs.
constexpr u32 kGdxsvCameraPlayerStride = 0x2000;
constexpr u32 kGdxsvCameraActorState = 4;		 // byte: 1 in play, 2 wrecked, 3 gone for a frame
constexpr u32 kGdxsvCameraActorPhase = 5;		 // byte: 1 while it arrives (drops in, then waits), 3 in control, 4 after the round
constexpr u32 kGdxsvCameraActorPosition = 0x20;	 // x, y, z of its feet
constexpr u16 kGdxsvCameraMoveAirborne = 0x0008;  // in its move flags: off the ground or a surface in space

// Where a disc of T13306M keeps what the god's eye view uses. The guest code gets its share when it is installed.
struct GdxsvCameraGame {
	u32 sign_addr, sign_value;	// a word of the game's code that tells this build of it
	// The literals the three calls the guest code takes over go through, and what the game has in them.
	u32 control_cell, stock_control;
	u32 pre_render_cell, stock_pre_render;
	u32 post_render_cell, stock_post_render;
	u32 sys_cell, sys2_cell;  // pointers to the system works
	u32 scene;				  // the scene id
	u32 camera_work, camera_stride;	 // the camera record of each view id
	u32 player_work;				 // the four players' actors
	u32 hud_list_head;				 // objects of the HUD pass, linked through +0x7c
	u32 water;						 // whether an eye is under water
	u32 label_trans, reticle_trans;	 // the renderers of the target label and the reticle
	// Offsets in the system work.
	u16 sys_demo[2];	   // set while the game shows a demo
	u16 sys_views;		   // the two views' matrix pointers
	u16 sys_view_id;	   // the view drawn
	u16 sys_water;		   // whether the view drawn is under water
	u16 sys_two_views;	   // set while the second view is drawn too
	u16 sys_battle_over;  // set when the mission text comes up
	// Offsets in a camera record: the eye, the point looked at, the view matrix.
	u16 cam_eye, cam_target, cam_matrix;
	// Offsets in an actor; a beyond-range flag of 0 is none.
	u16 actor_move_flags, actor_boost, actor_target, actor_beyond_range;

	u32 actor(int player) const { return player_work + player * kGdxsvCameraPlayerStride; }
};

// Emulator thread: the disc in memory, as gdxsv_camera_next_frame last found it.
const GdxsvCameraGame &gdxsv_camera_game();

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
	std::atomic<u32> replay{0};	   // replays and live battles started: what is read once per replay is read again
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
// The first player's buttons as the god's eye view reads them (pressed = bit clear), for the controls guide.
void gdxsv_camera_guide_buttons(u32 kcode);
// Render thread, each frame the CAMERA panel is drawn: the controls guide over the picture, shown while its ? is hovered.
void gdxsv_camera_render_guide(bool hovered);
// The aspect of the game's picture in a window of this aspect.
float gdxsv_camera_picture_aspect(float window_aspect);
// Called every frame a hand turns or tilts the view: its distance stays put until a little after the last call.
void gdxsv_camera_hold_distance();
// Puts the settings the viewer keeps in their options, for emu.cfg's next save.
void gdxsv_camera_keep_settings();
// Keeps them and saves emu.cfg if they changed: after an edit in the settings popup and when the replay ends.
void gdxsv_camera_save_settings();
