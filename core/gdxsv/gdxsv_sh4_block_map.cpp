// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// SH4 block map for profiling: with GDXSV_SH4_BLOCK_MAP=<file>, every block the dynarec compiles is appended to the
// file, so samples of host code can be mapped to guest addresses (gdxsv_tests/perf/rip_profile.py --block-map).
// Line: <host code> <host size> <guest addr> <guest instructions> <guest cycles> <reads> <writes> <fpu ops>
// <interpreter fallbacks> <temp block>. Host code is reused after a cache reset: a later line wins.
// Line for other dynarec code (main loop, memory handlers): <host code> <host size> <name>.
#include <cstdio>
#include <cstdlib>

#include "gdxsv_sh4_hooks.h"
#include "hw/sh4/dyna/blockmanager.h"

namespace {
const char* const blockMapPath = getenv("GDXSV_SH4_BLOCK_MAP");

FILE* blockMapFile() {
	static FILE* f = fopen(blockMapPath, "w");
	return f;
}

bool isFpu(shilop op) {
	switch (op) {
		case shop_fadd:
		case shop_fsub:
		case shop_fmul:
		case shop_fdiv:
		case shop_fabs:
		case shop_fneg:
		case shop_fsqrt:
		case shop_fipr:
		case shop_ftrv:
		case shop_fmac:
		case shop_fsrra:
		case shop_fsca:
		case shop_fseteq:
		case shop_fsetgt:
		case shop_cvt_f2i_t:
		case shop_cvt_i2f_n:
		case shop_cvt_i2f_z:
			return true;
		default:
			return false;
	}
}
}  // namespace

const bool gdxsv_sh4_block_map_on = blockMapPath != nullptr;

void gdxsv_sh4_block_compiled(const RuntimeBlockInfo* block) {
	FILE* f = blockMapFile();
	if (f == nullptr) return;
	int reads = 0, writes = 0, fpu = 0, fallbacks = 0;
	for (const shil_opcode& op : block->oplist) {
		reads += op.op == shop_readm;
		writes += op.op == shop_writem;
		fpu += isFpu(op.op);
		fallbacks += op.op == shop_ifb;
	}
	fprintf(f, "%p %u %08X %u %u %d %d %d %d %d\n", (const void*)block->code, block->host_code_size, block->addr, block->guest_opcodes,
			block->guest_cycles, reads, writes, fpu, fallbacks, block->temp_block ? 1 : 0);
	fflush(f);	// the profiled process is usually killed
}

void gdxsv_sh4_code_regions(const void* mainloop, const void* const* handlers, int count, const void* handlersEnd) {
	FILE* f = blockMapFile();
	if (f == nullptr) return;
	static const char* const typeNames[] = {"fast", "sq", "slow"};
	static const char* const opNames[] = {"read", "write"};
	const u8* start = (const u8*)mainloop;
	fprintf(f, "%p %zu mainloop\n", mainloop, (size_t)((const u8*)handlers[0] - start));
	for (int i = 0; i < count && i < 24; i++) {
		const u8* code = (const u8*)handlers[i];
		const u8* end = (const u8*)(i + 1 < count ? handlers[i + 1] : handlersEnd);
		if (end == code) continue;	// a store queue handler exists only for 32 and 64 bit writes
		fprintf(f, "%p %zu mem_%s_%s%d\n", code, (size_t)(end - code), typeNames[i / 8], opNames[i % 2], 8 << (i / 2 % 4));
	}
	fflush(f);
}
