// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Guest code of the replay god's eye view. Publishes a view the host computes, holds it in the camera record during the
// render call, and leaves the POV player's HUD out of it; the host draws the players' labels. Where the game keeps what this
// uses differs between the discs: the host writes the disc's own into gdx_cam.game when it installs this. Built by
// build_camera.sh into a blob of its own, so the released payload stays byte-identical.
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#define CAMDATA __attribute__((section("gdx.cam.data")))
#define CAMFUNC __attribute__((section("gdx.cam.func"), noinline))

#define R8(a) (*(volatile u8 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R32(a) (*(volatile u32 *)(a))

// The same on both discs.
#define VIEW_LAST 8	 // the last view id with a camera record: 0-3 the players' views, 5 + a player its own shots
#define SCENE_BATTLE 0x0702
#define HUD_LIST_MAX 64
#define COCKPIT_GAUGE 6	 // cockpit type: team gauge and the POV player's name

enum {
	ST_OK = 0,
	ST_SCENE,
	ST_DEMO,
	ST_ABSENT,
	ST_BAD_VIEW,
};

// A disc's addresses, and offsets in its system work and camera records.
struct gdx_game_t {
	u32 sys_cell;		   // +00 pointer to the system work
	u32 sys2_cell;		   // +04 pointer to the second system work
	u32 scene;			   // +08 the scene id, u16
	u32 camera_work;	   // +0c the camera record of each view id
	u32 camera_stride;	   // +10
	u32 hud_list_head;	   // +14 objects the HUD pass of render_current_frame walks, linked through +0x7c
	u32 stock_control;	   // +18 what the three hooked calls call
	u32 stock_pre_render;  // +1c
	u32 stock_post_render; // +20
	u32 water;			   // +24 whether an eye is under water
	u32 label_trans;	   // +28 the POV player's target label
	u32 reticle_trans;	   // +2c
	u16 sys_demo[2];	   // +30 system work: set while the game shows a demo
	u16 sys_views;		   // +34 the two views' matrix pointers
	u16 sys_view_id;	   // +36 the view drawn
	u16 sys_water;		   // +38 whether the view drawn is under water
	u16 sys_two_views;	   // +3a set while the second view is drawn too
	u16 cam_eye;		   // +3c camera record: the eye, x y z
	u16 cam_target;		   // +3e the point looked at
	u16 cam_matrix;		   // +40 the view matrix, 4x4
	u16 pad;			   // +42
};

struct gdx_cam_t {
	u32 patch_id;		   // +00 host, set by the installer
	u32 enabled;		   // +04 host
	u32 status;			   // +08
	u32 render_eye;		   // +0c
	u32 render_armed;	   // +10
	u32 saved_water;	   // +14
	u32 render_camera;	   // +18
	u32 swap_camera;	   // +1c
	u32 saved_camera[47];  // +20
	struct gdx_game_t game;	 // +dc host, set by the installer
};

struct gdx_cam_t gdx_cam CAMDATA = {0};

// The god's eye view. The host computes the view, the guest publishes it.
struct gdx_top_t {
	u32 ready;			// +000 host: camera holds this frame's values
	u32 hide;			// +004 host: bit per cockpit type to leave out of the HUD
	u32 camera[0x140 / 4];	// +008 host: a camera record of the disc, at most 0x140 bytes
	u32 hud_count;		// objects whose renderer is taken out for this render call
	u32 hud_obj[HUD_LIST_MAX];
	u32 hud_fn[HUD_LIST_MAX];
	u32 gauge_fn;  // the team gauge's renderer: the game's, or the widescreen HUD's in front of it
};

struct gdx_top_t gdx_top CAMDATA = {0};

static inline s32 cam_is_ram(u32 p) { return (p & 0xff000000) == 0x0c000000 || (p & 0xff000000) == 0x8c000000; }

static inline s32 cam_finite(u32 bits) { return ((bits >> 23) & 0xff) != 0xff; }

static void CAMFUNC cam_publish(u8 *sys, u8 *camera) {
	const struct gdx_game_t *g = &gdx_cam.game;
	R32(sys + g->sys_views) = (u32)(camera + g->cam_matrix);
	// The game's one-view path leaves the second slot alone.
	if (sys[g->sys_two_views] != 0) R32(sys + g->sys_views + 4) = (u32)(camera + g->cam_matrix);
	gdx_cam.render_eye = (u32)(camera + g->cam_eye);
	gdx_cam.render_camera = (u32)camera;
}

static s32 CAMFUNC cam_valid_view(const u8 *camera) {
	const struct gdx_game_t *g = &gdx_cam.game;
	s32 i;
	u32 any = 0;
	for (i = 0; i < 3; i++) {
		if (!cam_finite(R32(camera + g->cam_eye + i * 4))) return 0;
		if (!cam_finite(R32(camera + g->cam_target + i * 4))) return 0;
	}
	for (i = 0; i < 16; i++) {
		u32 v = R32(camera + g->cam_matrix + i * 4);
		if (!cam_finite(v)) return 0;
		any |= v;
	}
	return any != 0;
}

// The god's eye view shows no POV player, and the host draws a name of its own there. The renderer reads the name's length
// live, so the name sheet stays as it is.
static void CAMFUNC cam_gauge_trans(u8 *obj) {
	const u8 *sys2 = (const u8 *)R32(gdx_cam.game.sys2_cell);
	u8 *length = (u8 *)sys2 + 0x170 + (s8)sys2[6];
	const u8 stock = *length;
	*length = 0;
	((void (*)(u8 *))gdx_top.gauge_fn)(obj);
	*length = stock;
}

// Runs with the god's eye camera swapped in. All of it is undone by cam_hud_end at the end of the render call.
static void CAMFUNC cam_hud_begin(void) {
	const struct gdx_game_t *g = &gdx_cam.game;
	u32 obj;
	s32 walked = 0;

	gdx_top.hud_count = 0;
	for (obj = R32(g->hud_list_head); cam_is_ram(obj) && walked < 4 * HUD_LIST_MAX; obj = R32(obj + 0x7c), walked++) {
		const u8 *o = (const u8 *)obj;
		const u32 fn = R32(obj + 0x78);
		const s32 cockpit = o[1] == 0x14 && o[2] == 0;
		const s32 communication = o[1] == 0x14 && o[2] == 5;
		const s32 hide = fn == g->reticle_trans || fn == g->label_trans || (cockpit && o[3] < 32 && ((gdx_top.hide >> o[3]) & 1)) ||
						 (communication && gdx_top.hide != 0);
		const s32 gauge = cockpit && o[3] == COCKPIT_GAUGE;
		if ((hide || gauge) && fn != 0 && gdx_top.hud_count < HUD_LIST_MAX) {
			gdx_top.hud_obj[gdx_top.hud_count] = obj;
			gdx_top.hud_fn[gdx_top.hud_count] = fn;
			gdx_top.hud_count++;
			if (gauge && !hide) gdx_top.gauge_fn = fn;
			R32(obj + 0x78) = hide ? 0 : (u32)cam_gauge_trans;
		}
	}
}

static void CAMFUNC cam_hud_end(void) {
	u32 i;
	for (i = 0; i < gdx_top.hud_count; i++) R32(gdx_top.hud_obj[i] + 0x78) = gdx_top.hud_fn[i];
	gdx_top.hud_count = 0;
}

static u32 CAMFUNC cam_update(void) {
	const struct gdx_game_t *g = &gdx_cam.game;
	const u32 sys_p = R32(g->sys_cell);
	u8 *sys = (u8 *)sys_p;
	u8 *camera = (u8 *)gdx_top.camera;

	if (!cam_is_ram(sys_p) || !cam_is_ram(R32(g->sys2_cell))) return ST_SCENE;
	if (R16(g->scene) != SCENE_BATTLE) return ST_SCENE;
	if (sys[g->sys_demo[0]] != 0 || sys[g->sys_demo[1]] != 0) return ST_DEMO;
	if (!gdx_top.ready) return ST_ABSENT;
	if (!cam_valid_view(camera)) return ST_BAD_VIEW;
	cam_publish(sys, camera);
	return ST_OK;
}

// The game keeps drawing while the replay is paused, with no camera update in between, so what counts is that the
// view it is about to draw is still the one published here. Outside a battle the stage this relies on may be gone.
static inline s32 cam_view_is_published(const u8 *sys) {
	const struct gdx_game_t *g = &gdx_cam.game;
	if (R16(g->scene) != SCENE_BATTLE) return 0;
	if (!gdx_top.ready) return 0;
	return gdx_cam.render_camera != 0 && R32(sys + g->sys_views) == gdx_cam.render_camera + g->cam_matrix;
}

// The two calls around render_current_frame go through literals, so the call site stays as it is. Game logic
// outside the render call keeps the local player's view id, render-time consumers (culling, LOD) get the god's eye view.
void CAMFUNC gdx_cam_pre_render(void) {
	const struct gdx_game_t *g = &gdx_cam.game;
	((void (*)(void))g->stock_pre_render)();
	if (!cam_is_ram(R32(g->sys_cell))) return;
	if (gdx_cam.enabled && cam_view_is_published((const u8 *)R32(g->sys_cell))) {
		u8 *sys = (u8 *)R32(g->sys_cell);
		// Culling reads the record of the view id in use, which is not the local player's during the game's own shots.
		const u32 local = (s8)R8(R32(g->sys2_cell) + 6);
		const u32 view = sys[g->sys_view_id];
		u32 *dst = (u32 *)(g->camera_work + (view <= VIEW_LAST ? view : local) * g->camera_stride + 4);
		const u32 *src = (const u32 *)(gdx_cam.render_camera + 4);
		const int water = ((int (*)(void *))g->water)((void *)gdx_cam.render_eye);
		s32 i;
		for (i = 0; i < 47; i++) {
			gdx_cam.saved_camera[i] = dst[i];
			dst[i] = src[i];
		}
		gdx_cam.swap_camera = (u32)dst;
		gdx_cam.saved_water = sys[g->sys_water];
		sys[g->sys_water] = water;
		gdx_cam.render_armed = 1;
		cam_hud_begin();
	}
}

void CAMFUNC gdx_cam_post_render(void) {
	const struct gdx_game_t *g = &gdx_cam.game;
	if (gdx_cam.render_armed) {
		u8 *sys = (u8 *)R32(g->sys_cell);
		u32 *dst = (u32 *)gdx_cam.swap_camera;
		s32 i;
		cam_hud_end();
		for (i = 0; i < 47; i++) dst[i] = gdx_cam.saved_camera[i];
		gdx_cam.swap_camera = 0;
		sys[g->sys_water] = gdx_cam.saved_water;
		gdx_cam.render_armed = 0;
	}
	((void (*)(void))g->stock_post_render)();
}

void CAMFUNC gdx_cam_control(int local_index) {
	((void (*)(int))gdx_cam.game.stock_control)(local_index);
	if (!gdx_cam.enabled) return;
	gdx_cam.status = cam_update();
}
