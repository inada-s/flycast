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

// Game addresses (T13306M disc 2). The guest code takes over three call literals.
constexpr u32 kControlCell = 0x0c163384;  // literal of the per-frame control call
constexpr u32 kStockControl = 0x0c16631e;
constexpr u32 kPreRenderCell = 0x0c052164;	// literal of the call before render_current_frame
constexpr u32 kStockPreRender = 0x0c193fa6;
constexpr u32 kPostRenderCell = 0x0c052168;	 // literal of the call after it
constexpr u32 kStockPostRender = 0x0c17d3e0;
constexpr u32 kHandlerTable = 0x0c254d98;  // a handler table; its first entry tells this build of the game
constexpr u32 kHandlerTableFirst = 0x0c168738;
constexpr u32 kSysCell = 0x0c25c0f4;   // pointer to the game's system work
constexpr u32 kSysViewId = 0x68;	   // system work: the view drawn
constexpr u32 kSysBattleOver = 0x274;  // system work: set when the mission text comes up
constexpr u8 kViewIdShots = 5;		   // the game's own shots, 5 + the local player: before a battle, and the losing MS's end after it
constexpr u32 kScene = 0x0c3d16d4;	   // the game's scene id
constexpr u32 kSceneBattle = 0x0702;
constexpr u32 kCameraWork = 0x0c3dce00;	 // each player's own camera record
constexpr u32 kCameraStride = 0x140;
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
	Framing framing;
	bool installed = false;
	bool unsupported = false;
	bool fog_off = false;
	bool fog_saved = true;
	u32 status = ~0u;
};

CameraState cam;
// Set by the emulator thread each frame, read by the others.
std::atomic<bool> available{false};
// The kept settings differ from emu.cfg.
std::atomic<bool> unsaved{false};

s64 SteadyNanoseconds() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void WriteFloat(u32 addr, float value) {
	u32 bits;
	std::memcpy(&bits, &value, sizeof(bits));
	gdxsv_WriteMem32(addr, bits);
}

bool KnownGame() {
	const u32 control = gdxsv_ReadMem32(kControlCell);
	const u32 pre = gdxsv_ReadMem32(kPreRenderCell);
	const u32 post = gdxsv_ReadMem32(kPostRenderCell);
	return (control == kStockControl || control == kCamControl) && (pre == kStockPreRender || pre == kCamPreRender) &&
		   (post == kStockPostRender || post == kCamPostRender) && gdxsv_ReadMem32(kHandlerTable) == kHandlerTableFirst;
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
	gdxsv_WriteMem32(kCamData + kOffPatchId, kCamPatchId);
	NOTICE_LOG(COMMON, "God's eye view: guest code installed, patch %08x", kCamPatchId);
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

// Stage fog is tuned for battle distances and washes out a view from high above. Overridden while off, so never saved, and
// loaded again after, as at startup: the file's value with any override from the command line.
void ViewFog(bool off) {
	if (off == cam.fog_off) return;
	cam.fog_off = off;
	if (off) {
		cam.fog_saved = config::Fog;
		config::Fog.override(false);
	} else {
		config::Fog.reset();
		config::Fog.load();
		if (config::Fog != cam.fog_saved) config::Fog.set(cam.fog_saved);
	}
}

// A respawned MS drops in from high above its spawn point and counts once it has landed, which the game's own flag tells
// on any terrain. Its first frame up there still reads as on the ground.
bool Landed(int player, u32 actor) {
	int &landed = cam.framing.landed[player];
	const u8 phase = gdxsv_ReadMem8(actor + kGdxsvCameraActorPhase);
	const bool airborne = (gdxsv_ReadMem16(actor + kGdxsvCameraActorMoveFlags) & kGdxsvCameraMoveAirborne) != 0;
	if (phase != kPhaseArriving)
		landed = kLandedFrames;
	else if (cam.framing.phase[player] != kPhaseArriving || airborne)
		landed = 0;
	else
		landed = std::min(landed + 1, kLandedFrames);
	cam.framing.phase[player] = phase;
	return landed >= kLandedFrames;
}

void WriteView(const GdxsvCameraFrame &frame, float dist) {
	const GdxsvCameraVec3 &eye = frame.eye, &right = frame.right, &up = frame.up, &forward = frame.forward;
	const u32 camera = kCamTop + kTopCamera;
	// Header words as the game's watch camera init leaves them.
	gdxsv_WriteMem32(camera, 0x00000204);
	WriteFloat(camera + 4, 2.f);
	WriteFloat(camera + 0x10, eye.x);
	WriteFloat(camera + 0x14, eye.y);
	WriteFloat(camera + 0x18, eye.z);
	WriteFloat(camera + 0x28, eye.x + forward.x * dist);
	WriteFloat(camera + 0x2c, eye.y + forward.y * dist);
	WriteFloat(camera + 0x30, eye.z + forward.z * dist);
	WriteFloat(camera + 0x34, up.x);
	WriteFloat(camera + 0x38, up.y);
	WriteFloat(camera + 0x3c, up.z);
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
	for (int i = 0; i < 16; i++) {
		WriteFloat(camera + 0x40 + i * 4, view[i]);
		// The record's second matrix, which the god's eye view leaves unused.
		WriteFloat(camera + 0x80 + i * 4, i % 5 == 0 ? 1.f : 0.f);
	}
	gdxsv_WriteMem32(kCamTop + kTopHide, kTopHideMask);
	gdxsv_WriteMem32(kCamTop + kTopReady, 1);
}

// Frames the followed players, from above at the angle setting. Only guest memory of the guest code is written.
void UpdateView() {
	if (gdxsv_ReadMem16(kScene) != kSceneBattle) {
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
		const u32 actor = kGdxsvCameraPlayerWork + i * kGdxsvCameraPlayerStride;
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
		if (gdxsv_ReadMem8(kGdxsvCameraPlayerWork + i * kGdxsvCameraPlayerStride) != 0) present |= 1u << i;
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
	if (gdxsv_ReadMem16(kScene) != kSceneBattle) return false;
	const u32 sys = gdxsv_ReadMem32(kSysCell);
	if ((sys >> 24) != 0x0c || gdxsv_ReadMem8(sys + kSysBattleOver) == 0) return false;
	const u8 view = gdxsv_ReadMem8(sys + kSysViewId);
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

void gdxsv_camera_keep_settings() {
	const GdxsvCameraSettings &settings = gdxsv_camera_settings;
	const auto keep = [](auto &option, auto value) {
		if (option.get() == value) return;
		option.set(value);
		unsaved = true;
	};
	keep(config::GdxReplayGodsEyeClosest, settings.closest.load());
	keep(config::GdxReplayGodsEyeAngle, settings.angle.load());
	keep(config::GdxReplayGodsEyeNameplates, settings.nameplates.load());
	keep(config::GdxReplayGodsEyeBoost, settings.boost.load());
	keep(config::GdxReplayGodsEyeTeamColors, settings.team_colors.load());
	keep(config::GdxReplayGodsEyeMsImage, settings.ms_image.load());
	keep(config::GdxReplayGodsEyeFade, settings.fade.load());
	keep(config::GdxReplayGodsEyeTrail, settings.trail.load());
	keep(config::GdxReplayGodsEyeTrailSeconds, settings.trail_seconds.load());
	keep(config::GdxReplayGodsEyeDpad, settings.dpad.load());
}

void gdxsv_camera_save_settings() {
	gdxsv_camera_keep_settings();
	if (unsaved.exchange(false)) config::Settings::instance().save();
}

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

bool gdxsv_camera_supports(int disk) { return disk == 2 && KnownGame(); }

bool gdxsv_camera_available() { return available; }

bool gdxsv_camera_gods_eye() { return available && gdxsv_camera_settings.pov_mode == GdxsvPovMode::GodsEye; }

void gdxsv_camera_replay_started() {
	gdxsv_camera_settings.focus = kGdxsvCameraEveryone;
	gdxsv_camera_settings.zoom = 1.f;
	gdxsv_camera_settings.replay++;
}

void gdxsv_camera_replay_ended() { gdxsv_camera_save_settings(); }

float gdxsv_camera_player_camera(int player, u32 offset) {
	const u32 record = kCameraWork + player * kCameraStride;
	if (gdxsv_ReadMem32(kCamData + kOffSwapCamera) == record + 4) return gdxsv_camera_read_float(kCamData + kOffSavedCamera + offset - 4);
	return gdxsv_camera_read_float(record + offset);
}

void gdxsv_camera_next_frame(int disk, bool replaying) {
	const bool supported = replaying && gdxsv_camera_supports(disk);
	available = supported;
	const bool gods_eye = gdxsv_camera_settings.pov_mode == GdxsvPovMode::GodsEye;
	if (replaying && disk == 2 && !supported && gods_eye && !cam.unsupported) {
		WARN_LOG(COMMON, "God's eye view: unknown game build, control call %08x", gdxsv_ReadMem32(kControlCell));
		cam.unsupported = true;
	}
	const bool want = supported && gods_eye && !GameEndingShot();
	if (!want && !cam.installed) return;
	if (!supported) {
		ViewFog(false);
		return;
	}
	if (want && gdxsv_ReadMem32(kCamData + kOffPatchId) != kCamPatchId) {
		Install();
		cam.installed = true;
	}
	if (gdxsv_ReadMem32(kCamData + kOffPatchId) != kCamPatchId) return;

	gdxsv_WriteMem32(kCamData + kOffEnabled, want ? 1 : 0);
	if (want)
		UpdateView();
	else
		ViewOff();
	ViewFog(want);

	const u32 control = want ? kCamControl : kStockControl;
	if (gdxsv_ReadMem32(kControlCell) != control) {
		emu.getSh4Executor()->ResetCache();
		gdxsv_WriteMem32(kControlCell, control);
	}
	// The SH4 can stop inside the render call: the post-render hook stays until it has put the camera record back.
	const bool armed = gdxsv_ReadMem32(kCamData + kOffRenderArmed) != 0;
	const u32 pre = want ? kCamPreRender : kStockPreRender;
	const u32 post = want || armed ? kCamPostRender : kStockPostRender;
	if (gdxsv_ReadMem32(kPreRenderCell) != pre || gdxsv_ReadMem32(kPostRenderCell) != post) {
		emu.getSh4Executor()->ResetCache();
		gdxsv_WriteMem32(kPreRenderCell, pre);
		gdxsv_WriteMem32(kPostRenderCell, post);
	}
	if (want) LogStatus();
}

void gdxsv_camera_state_loaded() {
	if (cam.installed) emu.getSh4Executor()->ResetCache();
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
	cam.unsupported = false;
	cam.status = ~0u;
	available = false;
}
