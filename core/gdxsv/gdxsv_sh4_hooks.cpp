// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_sh4_hooks.h"

#include <algorithm>
#include <iterator>

#include "gdxsv_config.h"
#include "hw/sh4/sh4_if.h"

void gdxsv_sh4_block_cycles(u32 addr, u32& guest_cycles) {
	if (!config::GdxSlowIdleLoopHack) return;
	if (settings.gdxsv.disk == 1 && addr == 0x0c2317fa) {
		guest_cycles = 1448;
	}
	if (settings.gdxsv.disk == 2 && addr == 0x0c2ccdba) {
		guest_cycles = 1448;
	}
}

static u32 renderReturn;

// Most addresses are rejected with one bit test.
static u64 renderOnlyMask[64];
static const u32* renderOnlyMaskFor;

static void updateRenderOnlyMask() {
	if (renderOnlyMaskFor == settings.gdxsv.renderOnlyFuncs) return;
	std::fill(std::begin(renderOnlyMask), std::end(renderOnlyMask), 0);
	for (u32 i = 0; i < settings.gdxsv.renderOnlyFuncCount; i++) {
		const u32 bit = (settings.gdxsv.renderOnlyFuncs[i] >> 1) & 4095;
		renderOnlyMask[bit / 64] |= 1ull << (bit % 64);
	}
	renderOnlyMaskFor = settings.gdxsv.renderOnlyFuncs;
}

static bool isRenderOnlyFunc(u32 addr) {
	updateRenderOnlyMask();
	const u32 bit = (addr >> 1) & 4095;
	if (((renderOnlyMask[bit / 64] >> (bit % 64)) & 1) == 0) return false;
	for (u32 i = 0; i < settings.gdxsv.renderOnlyFuncCount; i++)
		if (settings.gdxsv.renderOnlyFuncs[i] == addr) return true;
	return false;
}

u32 gdxsv_sh4_code_addr(u32 addr) {
	if (settings.gdxsv.skipRenderingAddr == 0) {
		if (renderReturn != 0) renderReturn = 0;
	} else if (addr == settings.gdxsv.skipRenderingAddr) {
		updateRenderOnlyMask();
		renderReturn = addr + 4;
	} else if (renderReturn != 0) {
		if (isRenderOnlyFunc(addr)) {
			Sh4cntx.pc = Sh4cntx.pr;
			addr = Sh4cntx.pc;
		}
		if (addr == renderReturn) renderReturn = 0;
	}
	return addr;
}

bool gdxsv_sh4_linkable(u32 addr) {
	const u32 base = settings.gdxsv.skipRenderingBaseAddr;
	if (base == 0) return true;
	if (addr == base || addr == base + 4) return false;
	return !isRenderOnlyFunc(addr);
}

u32* gdxsv_sh4_render_return_ptr() { return &renderReturn; }

u64* gdxsv_sh4_render_only_mask_ptr() {
	updateRenderOnlyMask();
	return renderOnlyMask;
}
