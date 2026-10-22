// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Frame pacing probe (local measurement only, not for commit).
#pragma once
#include <cstdint>

#include <atomic>
extern bool gdxsv_fpt_on;
extern std::atomic<int64_t> gdxsv_fpt_last_vbl;
extern std::atomic<int64_t> gdxsv_fpt_vbl_period;
int64_t gdxsv_fpt_now();
void gdxsv_fpt_hr_sleep_until(int64_t t);
void gdxsv_fpt_emit(const char* ev, int64_t a, int64_t b, int64_t c);
inline void FPT(const char* ev, int64_t a = 0, int64_t b = 0, int64_t c = 0) {
	if (gdxsv_fpt_on) gdxsv_fpt_emit(ev, a, b, c);
}
