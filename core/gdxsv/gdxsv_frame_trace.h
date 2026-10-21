// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Frame timeline trace for measuring a rollback frame (investigation build).
// Off unless GDXSV_FRAME_TRACE=<csv path> is set. Rows: qpc,tid,frame,event,arg.
// frame is the GGPO frame last set by gdxsv_trace_set_frame (-1 outside GGPO).
// A helper thread also logs the host vblanks (IDXGIOutput::WaitForVBlank) as "hvbl".

extern bool gdxsv_trace_on;

void gdxsv_trace_init();
void gdxsv_trace_event(const char* event, long long arg);
void gdxsv_trace_set_frame(int frame);

// Press probe, also without the trace:
// GDXSV_PRESS=<player 0-3>,<frame>,<kcode bits hex>,<frames>: that peer holds those keys for those GGPO frames.
// GDXSV_RAM_DUMP=<dir>,<first>,<last>: main RAM at each GGPO save of those frames to <dir>/<frame>.bin.
// A rerun saves the frame again and overwrites the file, so it ends with the state of the final inputs.
// GDXSV_TA_HASH=1: log "ta_hash" (low 63 bits of XXH3 of the TA data) for each queued render.
extern bool gdxsv_trace_ta_hash;
void gdxsv_trace_press(int frame, int player, unsigned& kcode);
void gdxsv_trace_ram(int frame);

#define GDXSV_TRACE(event, arg)                \
	do {                                       \
		if (gdxsv_trace_on)                    \
			gdxsv_trace_event((event), (arg)); \
	} while (0)
