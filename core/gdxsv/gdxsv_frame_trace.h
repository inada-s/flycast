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

#define GDXSV_TRACE(event, arg)                \
	do {                                       \
		if (gdxsv_trace_on)                    \
			gdxsv_trace_event((event), (arg)); \
	} while (0)
