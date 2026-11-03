// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Guest code of the replay god's eye view, disc 2 only. Publishes a view the host computes, holds it in the camera record
// during the render call, and leaves the POV player's HUD out of it; the host draws the players' labels. Built by
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

// Game addresses (T13306M disc 2).
#define SYS_CELL 0x0c25c0f4
#define SYS2_CELL 0x0c25c0f8
#define CAMERA_WORK 0x0c3dce00
#define CAMERA_STRIDE 0x140
#define VIEW_LAST 8	 // the last view id with a camera record: 0-3 the players' views, 5 + a player its own shots
#define SCENE 0x0c3d16d4
#define SCENE_BATTLE 0x0702
#define HUD_LIST_HEAD 0x0c425098  // objects the HUD pass of render_current_frame walks, linked through +0x7c
#define HUD_LIST_MAX 64

#define STOCK_CONTROL ((void (*)(int))0x0c16631e)
#define STOCK_PRE_RENDER ((void (*)(void))0x0c193fa6)
#define STOCK_POST_RENDER ((void (*)(void))0x0c17d3e0)
#define WATER ((int (*)(void *))0x0c07b290)
#define LABEL_TRANS 0x0c120380	 // the POV player's target label
#define RETICLE_TRANS 0x0c12430a
#define COCKPIT_GAUGE 6	 // cockpit type: team gauge and the POV player's name

enum {
	ST_OK = 0,
	ST_SCENE,
	ST_DEMO,
	ST_ABSENT,
	ST_BAD_VIEW,
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
};

struct gdx_cam_t gdx_cam CAMDATA = {0};

// The god's eye view. The host computes the view, the guest publishes it.
struct gdx_top_t {
	u32 ready;						// +000 host: camera holds this frame's values
	u32 hide;						// +004 host: bit per cockpit type to leave out of the HUD
	u32 camera[CAMERA_STRIDE / 4];	// +008 host
	u32 hud_count;					// objects whose renderer is taken out for this render call
	u32 hud_obj[HUD_LIST_MAX];
	u32 hud_fn[HUD_LIST_MAX];
	u32 gauge_fn;  // the team gauge's renderer: the game's, or the widescreen HUD's in front of it
};

struct gdx_top_t gdx_top CAMDATA = {0};

static inline s32 cam_is_ram(u32 p) { return (p & 0xff000000) == 0x0c000000 || (p & 0xff000000) == 0x8c000000; }

static inline s32 cam_finite(u32 bits) { return ((bits >> 23) & 0xff) != 0xff; }

static void CAMFUNC cam_publish(u8 *sys, u8 *camera) {
	R32(sys + 0x60) = (u32)(camera + 0x40);
	// The game's one-view path leaves the second slot alone.
	if (sys[0x14a] != 0) R32(sys + 0x64) = (u32)(camera + 0x40);
	gdx_cam.render_eye = (u32)(camera + 0x10);
	gdx_cam.render_camera = (u32)camera;
}

static s32 CAMFUNC cam_valid_view(const u8 *camera) {
	s32 i;
	u32 any = 0;
	for (i = 0; i < 3; i++) {
		if (!cam_finite(R32(camera + 0x10 + i * 4))) return 0;
		if (!cam_finite(R32(camera + 0x28 + i * 4))) return 0;
	}
	for (i = 0; i < 16; i++) {
		u32 v = R32(camera + 0x40 + i * 4);
		if (!cam_finite(v)) return 0;
		any |= v;
	}
	return any != 0;
}

// The god's eye view shows no POV player, and the host draws a name of its own there. The renderer reads the name's length
// live, so the name sheet stays as it is.
static void CAMFUNC cam_gauge_trans(u8 *obj) {
	const u8 *sys2 = (const u8 *)R32(SYS2_CELL);
	u8 *length = (u8 *)sys2 + 0x170 + (s8)sys2[6];
	const u8 stock = *length;
	*length = 0;
	((void (*)(u8 *))gdx_top.gauge_fn)(obj);
	*length = stock;
}

// Runs with the god's eye camera swapped in. All of it is undone by cam_hud_end at the end of the render call.
static void CAMFUNC cam_hud_begin(void) {
	u32 obj;
	s32 walked = 0;

	gdx_top.hud_count = 0;
	for (obj = R32(HUD_LIST_HEAD); cam_is_ram(obj) && walked < 4 * HUD_LIST_MAX; obj = R32(obj + 0x7c), walked++) {
		const u8 *o = (const u8 *)obj;
		const u32 fn = R32(obj + 0x78);
		const s32 cockpit = o[1] == 0x14 && o[2] == 0;
		const s32 communication = o[1] == 0x14 && o[2] == 5;
		const s32 hide = fn == RETICLE_TRANS || fn == LABEL_TRANS || (cockpit && o[3] < 32 && ((gdx_top.hide >> o[3]) & 1)) ||
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
	const u32 sys_p = R32(SYS_CELL);
	u8 *sys = (u8 *)sys_p;
	u8 *camera = (u8 *)gdx_top.camera;

	if (!cam_is_ram(sys_p) || !cam_is_ram(R32(SYS2_CELL))) return ST_SCENE;
	if (R16(SCENE) != SCENE_BATTLE) return ST_SCENE;
	if (sys[0x59] != 0 || sys[0x5f] != 0) return ST_DEMO;
	if (!gdx_top.ready) return ST_ABSENT;
	if (!cam_valid_view(camera)) return ST_BAD_VIEW;
	cam_publish(sys, camera);
	return ST_OK;
}

// The game keeps drawing while the replay is paused, with no camera update in between, so what counts is that the
// view it is about to draw is still the one published here. Outside a battle the stage this relies on may be gone.
static inline s32 cam_view_is_published(const u8 *sys) {
	if (R16(SCENE) != SCENE_BATTLE) return 0;
	if (!gdx_top.ready) return 0;
	return gdx_cam.render_camera != 0 && R32(sys + 0x60) == gdx_cam.render_camera + 0x40;
}

// The two calls around render_current_frame go through literals, so the call site stays as it is. Game logic
// outside the render call keeps the local player's view id, render-time consumers (culling, LOD) get the god's eye view.
void CAMFUNC gdx_cam_pre_render(void) {
	STOCK_PRE_RENDER();
	if (!cam_is_ram(R32(SYS_CELL))) return;
	if (gdx_cam.enabled && cam_view_is_published((const u8 *)R32(SYS_CELL))) {
		u8 *sys = (u8 *)R32(SYS_CELL);
		// Culling reads the record of the view id in use, which is not the local player's during the game's own shots.
		const u32 local = (s8)R8(R32(SYS2_CELL) + 6);
		const u32 view = sys[0x68];
		u32 *dst = (u32 *)(CAMERA_WORK + (view <= VIEW_LAST ? view : local) * CAMERA_STRIDE + 4);
		const u32 *src = (const u32 *)(gdx_cam.render_camera + 4);
		const int water = WATER((void *)gdx_cam.render_eye);
		s32 i;
		for (i = 0; i < 47; i++) {
			gdx_cam.saved_camera[i] = dst[i];
			dst[i] = src[i];
		}
		gdx_cam.swap_camera = (u32)dst;
		gdx_cam.saved_water = sys[0x6a];
		sys[0x6a] = water;
		gdx_cam.render_armed = 1;
		cam_hud_begin();
	}
}

void CAMFUNC gdx_cam_post_render(void) {
	if (gdx_cam.render_armed) {
		u8 *sys = (u8 *)R32(SYS_CELL);
		u32 *dst = (u32 *)gdx_cam.swap_camera;
		s32 i;
		cam_hud_end();
		for (i = 0; i < 47; i++) dst[i] = gdx_cam.saved_camera[i];
		gdx_cam.swap_camera = 0;
		sys[0x6a] = gdx_cam.saved_water;
		gdx_cam.render_armed = 0;
	}
	STOCK_POST_RENDER();
}

void CAMFUNC gdx_cam_control(int local_index) {
	STOCK_CONTROL(local_index);
	if (!gdx_cam.enabled) return;
	gdx_cam.status = cam_update();
}
