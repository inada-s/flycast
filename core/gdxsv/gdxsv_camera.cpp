// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_camera.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

#include "cfg/option.h"
#include "emulator.h"
#include "gdxsv_camera_art.h"
#include "gdxsv_camera_internal.h"
#include "gdxsv_config.h"
#include "libs.h"
#include "log/Log.h"

GdxsvCameraSettings gdxsv_camera_settings;

namespace {
#include "gdxsv_camera_blob.inc"

// Disc 2 (DX). The control call is the per-frame one; the two render calls are the ones around render_current_frame.
GdxsvCameraGame Disc2() {
	GdxsvCameraGame g{};
	g.sign_addr = 0x0c254d98;  // a handler table, by its first entry
	g.sign_value = 0x0c168738;
	g.control_cell = 0x0c163384;
	g.stock_control = 0x0c16631e;
	g.pre_render_cell = 0x0c052164;
	g.stock_pre_render = 0x0c193fa6;
	g.post_render_cell = 0x0c052168;
	g.stock_post_render = 0x0c17d3e0;
	g.sys_cell = 0x0c25c0f4;
	g.sys2_cell = 0x0c25c0f8;
	g.scene = 0x0c3d16d4;
	g.camera_work = 0x0c3dce00;
	g.camera_stride = 0x140;
	g.player_work = 0x0c3d1cd4;
	g.hud_list_head = 0x0c425098;
	g.water = 0x0c07b290;
	g.label_trans = 0x0c120380;
	g.reticle_trans = 0x0c12430a;
	g.sys_demo[0] = 0x59;
	g.sys_demo[1] = 0x5f;
	g.sys_views = 0x60;
	g.sys_view_id = 0x68;
	g.sys_water = 0x6a;
	g.sys_two_views = 0x14a;
	g.sys_battle_over = 0x274;
	g.cam_eye = 0x10;
	g.cam_target = 0x28;
	g.cam_matrix = 0x40;
	g.actor_move_flags = 0x23c;
	g.actor_boost = 0x23e;
	g.actor_target = 0x32c;
	g.actor_beyond_range = 0x4ec;
	return g;
}

// Disc 1 (MUJI): the same functions elsewhere, and a camera record of its own layout.
GdxsvCameraGame Disc1() {
	GdxsvCameraGame g{};
	g.sign_addr = 0x0c113a48;  // the control call's literal of the system work pointer
	g.sign_value = 0x0c1db03c;
	g.control_cell = 0x0c110510;
	g.stock_control = 0x0c11390c;
	g.pre_render_cell = 0x0c064d50;
	g.stock_pre_render = 0x0c13496e;
	g.post_render_cell = 0x0c064d54;
	g.stock_post_render = 0x0c1264e8;
	g.sys_cell = 0x0c1db03c;
	g.sys2_cell = 0x0c1db040;
	g.scene = 0x0c336254;
	g.camera_work = 0x0c3413a4;
	g.camera_stride = 0xc0;
	g.player_work = 0x0c336854;
	g.hud_list_head = 0x0c387d3c;
	g.water = 0x0c084da4;
	g.label_trans = 0x0c01e9b0;
	g.reticle_trans = 0x0c0de10c;
	g.sys_demo[0] = 0x66;
	g.sys_demo[1] = 0x7c;
	g.sys_views = 0x40;
	g.sys_view_id = 0x4a;
	g.sys_water = 0x4c;
	g.sys_two_views = 0x17b;
	g.sys_battle_over = 0x262;
	g.cam_eye = 0x04;
	g.cam_target = 0x10;
	g.cam_matrix = 0x80;
	g.actor_move_flags = 0x22e;
	g.actor_boost = 0x230;
	g.actor_target = 0x31c;
	g.actor_beyond_range = 0;  // none: the game marks a target beyond range only in space, and Disc 1 has no space stages
	return g;
}

const GdxsvCameraGame kDisc1 = Disc1();
const GdxsvCameraGame kDisc2 = Disc2();

const GdxsvCameraGame *GameOf(int disk) { return disk == 1 ? &kDisc1 : disk == 2 ? &kDisc2 : nullptr; }

constexpr u8 kViewIdShots = 5;	// the game's own shots, 5 + the local player: before a battle, and the losing MS's end after it
constexpr u32 kSceneBattle = 0x0702;
constexpr u8 kStateGone = 3;
constexpr u8 kPhaseArriving = 1;
constexpr u8 kPhaseControl = 3;

// Offsets in gdx_cam_t of camera.c.
enum : u32 {
	kOffPatchId = 0x00,
	kOffEnabled = 0x04,
	kOffStatus = 0x08,
	kOffRenderArmed = 0x10,
	kOffSwapCamera = 0x1c,
	kOffSavedCamera = 0x20,
	kOffGame = 0xdc,
};

// Offsets in gdx_top_t of camera.c.
enum : u32 {
	kTopReady = 0x00,
	kTopHide = 0x04,
	kTopCamera = 0x08,
};

// Cockpit types the god's eye view keeps, a bit each: 0 the timer, 6 the team gauge. The others are about the POV player.
constexpr u32 kTopHideMask = ~((1u << 0) | (1u << 6));

// The framing. The projection reaches kTanV vertically per unit of distance, horizontally that times its aspect.
// Players stay out of the top of the screen, where the timer and gauge are and nameplates grow upward, and of the
// right edge, where nameplates grow to.
constexpr float kPad = 30.f;
constexpr float kTanV = 0.375f;
constexpr float kReserveRight = 0.12f;
constexpr float kReserveUp = 0.2f;
// Per frame, the share of the way to the framing.
constexpr float kFollow = 0.08f;
constexpr float kFollowHeight = 0.02f;
constexpr float kZoomOut = 0.08f;
constexpr float kZoomIn = 0.02f;
constexpr float kZoomNearest = 40.f;  // world units: the viewer's zoom brings the eye no nearer
constexpr int kLandedFrames = 6;	  // an arriving MS counts for the framing once it has stood this long
constexpr s64 kHandNanoseconds = 100'000'000;
constexpr float kRadiansPerDegree = 3.14159265f / 180.f;

struct Framing {
	bool live = false;
	GdxsvCameraVec3 center;
	float dist = 0.f;
	float yaw = 0.f, angle = 0.f, closest = 0.f;  // the settings it is framed for
	int landed[4] = {};							  // frames each arriving MS has stood on the ground
	u8 phase[4] = {};
};

struct CameraState {
	const GdxsvCameraGame *game = &kDisc2;
	bool supported = false;		   // the replay's game can show the view; read once per replay
	bool checked_replaying = false;  // what supported was read for
	u32 checked_replay = ~0u;
	Framing framing;
	bool installed = false;	 // the guest code is in RAM
	bool used = false;		 // it was in this replay: its states can bring it back, the code cache can hold blocks of it
	bool fog_off = false;
	bool fog = true;			// the fog option as the settings showed it when the view turned it off
	bool fog_read_only = false;	// and whether the settings could change it there (not when the command line sets it)
	u32 status = ~0u;
};

CameraState cam;
// Set by the emulator thread, read by the others.
std::atomic<bool> available{false};

s64 SteadyNanoseconds() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void WriteFloat(u32 addr, float value) {
	u32 bits;
	std::memcpy(&bits, &value, sizeof(bits));
	gdxsv_WriteMem32(addr, bits);
}

bool KnownGame(const GdxsvCameraGame &game) {
	const u32 control = gdxsv_ReadMem32(game.control_cell);
	const u32 pre = gdxsv_ReadMem32(game.pre_render_cell);
	const u32 post = gdxsv_ReadMem32(game.post_render_cell);
	return (control == game.stock_control || control == kCamControl) && (pre == game.stock_pre_render || pre == kCamPreRender) &&
		   (post == game.stock_post_render || post == kCamPostRender) && gdxsv_ReadMem32(game.sign_addr) == game.sign_value;
}

// gdx_game_t of camera.c.
void WriteGuestGame(const GdxsvCameraGame &game) {
	const u32 t = kCamData + kOffGame;
	const u32 words[] = {game.sys_cell,		 game.sys2_cell,	   game.scene,			 game.camera_work,
						 game.camera_stride, game.hud_list_head, game.stock_control, game.stock_pre_render,
						 game.stock_post_render, game.water,	   game.label_trans,	 game.reticle_trans};
	for (size_t i = 0; i < std::size(words); i++) gdxsv_WriteMem32(t + static_cast<u32>(i) * 4, words[i]);
	const u16 halves[] = {game.sys_demo[0],		game.sys_demo[1], game.sys_views,	  game.sys_view_id, game.sys_water,
						  game.sys_two_views, game.cam_eye,		  game.cam_target, game.cam_matrix};
	for (size_t i = 0; i < std::size(halves); i++) gdxsv_WriteMem16(t + 0x30 + static_cast<u32>(i) * 2, halves[i]);
}

void Install() {
	emu.getSh4Executor()->ResetCache();
	for (size_t i = 0; i < std::size(kCamCodeWords); i++) {
		gdxsv_WriteMem16(0x80000000u + kCamCode + static_cast<u32>(i) * 2, kCamCodeWords[i]);
	}
	for (u32 i = 0; i < kCamDataSize; i += 4) {
		gdxsv_WriteMem32(std::min(kCamData, kCamTop) + i, 0);
	}
	cam.framing.live = false;
	WriteGuestGame(*cam.game);
	gdxsv_WriteMem32(kCamData + kOffPatchId, kCamPatchId);
	cam.installed = true;
	cam.used = true;
	NOTICE_LOG(COMMON, "God's eye view: guest code installed, patch %08x", kCamPatchId);
}

// With the hooks already back to the game's: the guest code's RAM, which the game leaves unused, is cleared again, so that
// a takeover and anything after the replay go on without it.
void Uninstall() {
	emu.getSh4Executor()->ResetCache();
	for (size_t i = 0; i < std::size(kCamCodeWords); i++) gdxsv_WriteMem16(0x80000000u + kCamCode + static_cast<u32>(i) * 2, 0);
	for (u32 i = 0; i < kCamDataSize; i += 4) gdxsv_WriteMem32(std::min(kCamData, kCamTop) + i, 0);
	cam.installed = false;
	cam.framing.live = false;
	NOTICE_LOG(COMMON, "God's eye view: guest code removed");
}

// The aspect the game's culling and projection have opened up to: only the widescreen hacks draw beyond 4:3.
float GameAspect() {
	if (!config::Widescreen || !config::WidescreenGameHacks || settings.display.height <= 0) return 4.f / 3.f;
	return gdxsv_camera_picture_aspect(static_cast<float>(settings.display.width) / settings.display.height);
}

void ViewOff() {
	cam.framing.live = false;
	gdxsv_WriteMem32(kCamTop + kTopReady, 0);
	gdxsv_camera_overlay_clear();
}

// Stage fog is tuned for battle distances and washes out a view from high above. Overridden while off, so never saved, then
// back to what the settings showed.
void ViewFog(bool off) {
	if (off == cam.fog_off) return;
	cam.fog_off = off;
	if (off) {
		cam.fog = config::Fog;
		cam.fog_read_only = config::Fog.isReadOnly();
		config::Fog.override(false);
	} else {
		config::Fog.reset();
		if (cam.fog_read_only)
			config::Fog.override(cam.fog);
		else
			config::Fog.set(cam.fog);
	}
}

// A respawned MS drops in from high above its spawn point and counts once it has landed, which the game's own flag tells
// on any terrain. Its first frame up there still reads as on the ground.
bool Landed(int player, u32 actor) {
	int &landed = cam.framing.landed[player];
	const u8 phase = gdxsv_ReadMem8(actor + kGdxsvCameraActorPhase);
	const bool airborne = (gdxsv_ReadMem16(actor + cam.game->actor_move_flags) & kGdxsvCameraMoveAirborne) != 0;
	if (phase != kPhaseArriving)
		landed = kLandedFrames;
	else if (cam.framing.phase[player] != kPhaseArriving || airborne)
		landed = 0;
	else
		landed = std::min(landed + 1, kLandedFrames);
	cam.framing.phase[player] = phase;
	return landed >= kLandedFrames;
}

void WriteVec3(u32 addr, const GdxsvCameraVec3 &v) {
	WriteFloat(addr, v.x);
	WriteFloat(addr + 4, v.y);
	WriteFloat(addr + 8, v.z);
}

void WriteView(const GdxsvCameraFrame &frame, float dist) {
	const GdxsvCameraGame &game = *cam.game;
	const GdxsvCameraVec3 &eye = frame.eye, &right = frame.right, &up = frame.up, &forward = frame.forward;
	const u32 camera = kCamTop + kTopCamera;
	if (&game == &kDisc2) {
		// Header words as the game's watch camera init leaves them, the up vector, and the record's second matrix, which the
		// god's eye view leaves unused.
		gdxsv_WriteMem32(camera, 0x00000204);
		WriteFloat(camera + 4, 2.f);
		WriteVec3(camera + 0x34, up);
		for (int i = 0; i < 16; i++) WriteFloat(camera + 0x80 + i * 4, i % 5 == 0 ? 1.f : 0.f);
	} else {
		// Disc 1's record also holds its camera's motion and lens: those of the POV player's, which the game keeps up.
		const u32 sys2 = gdxsv_ReadMem32(game.sys2_cell);
		const int local = std::clamp<int>(static_cast<s8>(gdxsv_ReadMem8(sys2 + 6)), 0, 3);
		const u32 own = game.camera_work + local * game.camera_stride;
		for (u32 i = 0; i < game.camera_stride; i += 4) gdxsv_WriteMem32(camera + i, gdxsv_ReadMem32(own + i));
	}
	WriteVec3(camera + game.cam_eye, eye);
	WriteVec3(camera + game.cam_target, {eye.x + forward.x * dist, eye.y + forward.y * dist, eye.z + forward.z * dist});
	// Column major, as the game stores its views: rows right, up and back.
	const float view[16] = {
		right.x,
		up.x,
		-forward.x,
		0.f,  //
		right.y,
		up.y,
		-forward.y,
		0.f,  //
		right.z,
		up.z,
		-forward.z,
		0.f,  //
		-gdxsv_camera_dot(right, eye),
		-gdxsv_camera_dot(up, eye),
		gdxsv_camera_dot(forward, eye),
		1.f,
	};
	for (int i = 0; i < 16; i++) WriteFloat(camera + game.cam_matrix + i * 4, view[i]);
	gdxsv_WriteMem32(kCamTop + kTopHide, kTopHideMask);
	gdxsv_WriteMem32(kCamTop + kTopReady, 1);
}

// Frames the followed players, from above at the angle setting. Only guest memory of the guest code is written.
void UpdateView() {
	const GdxsvCameraGame &game = *cam.game;
	if (gdxsv_ReadMem16(game.scene) != kSceneBattle) {
		ViewOff();
		return;
	}
	const GdxsvCameraSettings &settings = gdxsv_camera_settings;
	Framing &framing = cam.framing;
	GdxsvCameraFrame frame;
	// Before any MS is in control, at a round's start, every MS drops in together and the drop is shown: all of them count
	// as landed, and the framing's height keeps up with them.
	bool round_start = true;
	for (int i = 0; i < 4; i++) {
		const u32 actor = game.actor(i);
		if (gdxsv_ReadMem8(actor) == 0 || gdxsv_ReadMem8(actor + kGdxsvCameraActorState) == kStateGone) continue;
		GdxsvCameraFrame::Player &p = frame.players[i];
		const u32 position = actor + kGdxsvCameraActorPosition;
		p.at = {gdxsv_camera_read_float(position), gdxsv_camera_read_float(position + 4), gdxsv_camera_read_float(position + 8)};
		if (!std::isfinite(p.at.x) || !std::isfinite(p.at.y) || !std::isfinite(p.at.z)) continue;
		p.on = true;
		p.landed = Landed(i, actor);
		if (gdxsv_ReadMem8(actor + kGdxsvCameraActorPhase) == kPhaseControl) round_start = false;
	}
	if (round_start) {
		for (GdxsvCameraFrame::Player &p : frame.players) p.landed = p.on;
	}
	// The followed players once they have landed. With none of them down the view holds still, unless there is no view
	// yet: then it takes them as they come. Followed players this battle does not have count as everyone.
	u32 present = 0;
	for (int i = 0; i < 4; i++) {
		if (gdxsv_ReadMem8(game.actor(i)) != 0) present |= 1u << i;
	}
	const u32 focus = (settings.focus & present) != 0 ? settings.focus & present : present;
	u32 framed = 0, arriving = 0;
	for (int i = 0; i < 4; i++) {
		if (!frame.players[i].on || !(focus >> i & 1)) continue;
		(frame.players[i].landed ? framed : arriving) |= 1u << i;
	}
	if (framed == 0 && !framing.live) framed = arriving;
	if (framed == 0 && !framing.live) {
		ViewOff();
		return;
	}
	frame.aspect = GameAspect();
	const float tan_h = kTanV * frame.aspect;

	// While a hand turns or tilts the view its distance stays put, so that it does not pump through a full turn, and
	// glides to the framing once let go. Any other new turn, angle or closest distance frames at once.
	const float yaw = settings.yaw;
	const float angle = settings.angle;
	const float closest = settings.closest;
	const bool hand = SteadyNanoseconds() < settings.hand_until;
	const bool snap = !hand && (framing.yaw != yaw || framing.angle != angle || framing.closest != closest);

	// Screen up on the ground turned by the yaw, leaning back from straight down by the angle.
	const float heading = yaw * kRadiansPerDegree;
	const float pitch = angle * kRadiansPerDegree;
	const GdxsvCameraVec3 ground_up{std::cos(heading), 0.f, std::sin(heading)};
	frame.right = {-ground_up.z, 0.f, ground_up.x};
	frame.forward = {ground_up.x * std::sin(pitch), -std::cos(pitch), ground_up.z * std::sin(pitch)};
	frame.up = {ground_up.x * std::cos(pitch), std::sin(pitch), ground_up.z * std::cos(pitch)};

	if (framed != 0) {
		float min_x = INFINITY, max_x = -INFINITY, min_z = INFINITY, max_z = -INFINITY, sum_y = 0.f;
		int count = 0;
		for (int i = 0; i < 4; i++) {
			if (!(framed >> i & 1)) continue;
			const GdxsvCameraVec3 &at = frame.players[i].at;
			min_x = std::min(min_x, at.x);
			max_x = std::max(max_x, at.x);
			min_z = std::min(min_z, at.z);
			max_z = std::max(max_z, at.z);
			sum_y += at.y;
			count++;
		}
		// Half an MS above their feet: a close view centres on the bodies.
		const GdxsvCameraVec3 center{(min_x + max_x) * 0.5f, sum_y / count + kGdxsvCameraMsHeight * 0.5f, (min_z + max_z) * 0.5f};
		// Each player fits at its own depth: in perspective the near ones take more room.
		float dist = closest;
		for (int i = 0; i < 4; i++) {
			if (!(framed >> i & 1)) continue;
			const GdxsvCameraVec3 &at = frame.players[i].at;
			const GdxsvCameraVec3 d{at.x - center.x, at.y - center.y, at.z - center.z};
			const float depth = gdxsv_camera_dot(d, frame.forward);
			dist = std::max({dist, (std::fabs(gdxsv_camera_dot(d, frame.right)) + kPad) / (tan_h * (1.f - kReserveRight)) - depth,
							 (std::fabs(gdxsv_camera_dot(d, frame.up)) + kPad) / (kTanV * (1.f - kReserveUp)) - depth});
		}
		if (!framing.live) {
			framing.live = true;
			framing.center = center;
			framing.dist = dist;
		} else {
			framing.center.x += (center.x - framing.center.x) * kFollow;
			framing.center.y += (center.y - framing.center.y) * (round_start ? kFollow : kFollowHeight);
			framing.center.z += (center.z - framing.center.z) * kFollow;
			if (snap)
				framing.dist = dist;
			else if (!hand)
				framing.dist += (dist - framing.dist) * (dist > framing.dist ? kZoomOut : kZoomIn);
		}
		framing.yaw = yaw;
		framing.angle = angle;
		framing.closest = closest;
	}

	// The players' middle sits ahead of the eye, moved off the screen's centre by the reserves.
	const float dist = std::max(framing.dist * settings.zoom, kZoomNearest);
	const float shift_right = tan_h * kReserveRight * dist;
	const float shift_up = kTanV * kReserveUp * dist;
	const GdxsvCameraVec3 &f = frame.forward, &r = frame.right, &u = frame.up;
	frame.eye = {framing.center.x - f.x * dist + r.x * shift_right + u.x * shift_up, framing.center.y - f.y * dist + u.y * shift_up,
				 framing.center.z - f.z * dist + r.z * shift_right + u.z * shift_up};
	WriteView(frame, dist);
	gdxsv_camera_overlay_update(frame);
}

// After the mission text the game shows the end of the losing MS, through its filter: the god's eye view leaves that to it.
bool GameEndingShot() {
	const GdxsvCameraGame &game = *cam.game;
	if (gdxsv_ReadMem16(game.scene) != kSceneBattle) return false;
	const u32 sys = gdxsv_ReadMem32(game.sys_cell);
	if ((sys >> 24) != 0x0c || gdxsv_ReadMem8(sys + game.sys_battle_over) == 0) return false;
	const u8 view = gdxsv_ReadMem8(sys + game.sys_view_id);
	return view >= kViewIdShots && view < kViewIdShots + 4;
}

// Why the guest code does not show the god's eye view, when that changes.
void LogStatus() {
	static const char *const kNames[] = {"shown", "not in battle", "demo", "no view yet", "bad view"};
	const u32 status = gdxsv_ReadMem32(kCamData + kOffStatus);
	if (status == cam.status) return;
	cam.status = status;
	NOTICE_LOG(COMMON, "God's eye view: %s", status < std::size(kNames) ? kNames[status] : "unknown status");
}

void LoadSettings() {
	GdxsvCameraSettings &settings = gdxsv_camera_settings;
	settings.closest = std::clamp(config::GdxReplayGodsEyeClosest.get(), kGdxsvCameraClosestMin, kGdxsvCameraClosestMax);
	settings.angle = std::clamp(config::GdxReplayGodsEyeAngle.get(), 0.f, kGdxsvCameraAngleMax);
	settings.nameplates = config::GdxReplayGodsEyeNameplates.get();
	settings.boost = config::GdxReplayGodsEyeBoost.get();
	settings.team_colors = config::GdxReplayGodsEyeTeamColors.get();
	settings.ms_image = config::GdxReplayGodsEyeMsImage.get();
	settings.fade = config::GdxReplayGodsEyeFade.get();
	settings.trail = config::GdxReplayGodsEyeTrail.get();
	settings.trail_seconds = std::clamp(config::GdxReplayGodsEyeTrailSeconds.get(), 1.f, kGdxsvCameraTrailSecondsMax);
	settings.dpad = config::GdxReplayGodsEyeDpad.get();
}
}  // namespace

float gdxsv_camera_picture_aspect(float window_aspect) {
	if (!config::Widescreen) return 4.f / 3.f * config::ScreenStretching / 100.f;
	return config::SuperWidescreen ? std::max(window_aspect, 4.f / 3.f) : 16.f / 9.f;
}

float gdxsv_camera_read_float(u32 addr) {
	const u32 bits = gdxsv_ReadMem32(addr);
	float value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void gdxsv_camera_hold_distance() { gdxsv_camera_settings.hand_until = SteadyNanoseconds() + kHandNanoseconds; }

void gdxsv_camera_set_pov_mode(GdxsvPovMode mode) { gdxsv_camera_settings.pov_mode = mode; }

bool gdxsv_camera_set_pov_mode_name(const std::string &name) {
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower == "player")
		gdxsv_camera_set_pov_mode(GdxsvPovMode::Player);
	else if (lower == "godseye")
		gdxsv_camera_set_pov_mode(GdxsvPovMode::GodsEye);
	else
		return false;
	return true;
}

GdxsvPovMode gdxsv_camera_pov_mode() { return gdxsv_camera_settings.pov_mode; }

const char *gdxsv_camera_pov_mode_label(GdxsvPovMode mode) { return mode == GdxsvPovMode::GodsEye ? "God's Eye View" : ""; }

bool gdxsv_camera_supports(int disk) {
	const GdxsvCameraGame *game = GameOf(disk);
	return game != nullptr && KnownGame(*game);
}

const GdxsvCameraGame &gdxsv_camera_game() { return *cam.game; }

bool gdxsv_camera_available() { return available; }

bool gdxsv_camera_gods_eye() { return available && gdxsv_camera_settings.pov_mode == GdxsvPovMode::GodsEye; }

void gdxsv_camera_replay_started() {
	gdxsv_camera_settings.focus = kGdxsvCameraEveryone;
	gdxsv_camera_settings.zoom = 1.f;
	gdxsv_camera_settings.replay++;
}

float gdxsv_camera_player_camera(int player, u32 offset) {
	const u32 record = cam.game->camera_work + player * cam.game->camera_stride;
	if (gdxsv_ReadMem32(kCamData + kOffSwapCamera) == record + 4) return gdxsv_camera_read_float(kCamData + kOffSavedCamera + offset - 4);
	return gdxsv_camera_read_float(record + offset);
}

void gdxsv_camera_next_frame(int disk, bool replaying) {
	// Once per replay: its game's code stays as it is through it.
	const u32 replay = gdxsv_camera_settings.replay;
	if (replaying != cam.checked_replaying || replay != cam.checked_replay) {
		cam.checked_replaying = replaying;
		cam.checked_replay = replay;
		const GdxsvCameraGame *game = replaying ? GameOf(disk) : nullptr;
		cam.supported = game != nullptr && KnownGame(*game);
		if (cam.supported) cam.game = game;
		if (game != nullptr && !cam.supported) NOTICE_LOG(COMMON, "God's eye view: unknown build of disc %d", disk);
		available = cam.supported;
		// What the view left in the last replay went with the state loaded after it.
		if (!cam.installed) cam.used = false;
	}
	const bool gods_eye = gdxsv_camera_settings.pov_mode == GdxsvPovMode::GodsEye;
	const bool want = cam.supported && gods_eye && !GameEndingShot();
	if (!want && !cam.installed) {
		// The first frame after a replay with the view: the state loaded after it took the guest code, not the fog.
		ViewFog(false);
		return;
	}
	if (!cam.supported) {
		ViewFog(false);
		return;
	}
	const GdxsvCameraGame *game = cam.game;
	if (want && !cam.installed) Install();

	gdxsv_WriteMem32(kCamData + kOffEnabled, want ? 1 : 0);
	if (want)
		UpdateView();
	else
		ViewOff();
	ViewFog(want);

	const u32 control = want ? kCamControl : game->stock_control;
	if (gdxsv_ReadMem32(game->control_cell) != control) {
		emu.getSh4Executor()->ResetCache();
		gdxsv_WriteMem32(game->control_cell, control);
	}
	// The SH4 can stop inside the render call: the post-render hook stays until it has put the camera record back.
	const bool armed = gdxsv_ReadMem32(kCamData + kOffRenderArmed) != 0;
	const u32 pre = want ? kCamPreRender : game->stock_pre_render;
	const u32 post = want || armed ? kCamPostRender : game->stock_post_render;
	if (gdxsv_ReadMem32(game->pre_render_cell) != pre || gdxsv_ReadMem32(game->post_render_cell) != post) {
		emu.getSh4Executor()->ResetCache();
		gdxsv_WriteMem32(game->pre_render_cell, pre);
		gdxsv_WriteMem32(game->post_render_cell, post);
	}
	if (want) LogStatus();
	// Back in the player's view, once the render call has put the camera record back.
	if (!gods_eye && !armed) Uninstall();
}

void gdxsv_camera_state_loaded() {
	// A state from while the view was up brings the guest code and its hooks back, and the code cache can hold blocks
	// compiled from them. Nothing to do in a replay without the view, or once a state from before it is loaded.
	if (!cam.used) return;
	emu.getSh4Executor()->ResetCache();
	cam.installed = gdxsv_ReadMem32(kCamData + kOffPatchId) == kCamPatchId;
}

void gdxsv_camera_reset() {
	GdxsvCameraSettings &settings = gdxsv_camera_settings;
	settings.pov_mode = GdxsvPovMode::Player;
	settings.yaw = 0.f;
	settings.zoom = 1.f;
	settings.focus = kGdxsvCameraEveryone;
	LoadSettings();
	gdxsv_camera_art_reset();
	gdxsv_camera_overlay_clear();
	gdxsv_camera_panel_reset();
	ViewFog(false);
	cam.framing = Framing{};
	cam.installed = false;
	cam.used = false;
	cam.supported = false;
	cam.checked_replaying = false;
	cam.checked_replay = ~0u;
	cam.status = ~0u;
	available = false;
}
