// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

// Session lifetime. The ordinary audio backend is restored on close.
bool gdxsv_multi_pov_audio_open();
void gdxsv_multi_pov_audio_close();
bool gdxsv_multi_pov_audio_enabled();
void gdxsv_multi_pov_audio_start();

// Called on the emulation thread after each run, before replay controls can
// change its position. Silent speed-burst frames advance the output delay too.
// Paused steps keep the audio clocks aligned across changes of replay input.
void gdxsv_multi_pov_audio_frame(int input, uint32_t round, uint32_t seek, int speed, bool silent, bool menu = false, bool paused = false);
void gdxsv_multi_pov_audio_reset();

// Briefing input requests are retried without stopping emulation or audio.
// Waiting expires after 350ms; resets discard any pending request.
bool gdxsv_multi_pov_audio_input_pending();
bool gdxsv_multi_pov_audio_input_ready(int input, uint32_t round, uint32_t seek, bool wait);

// Session-owned, optional PCM mapping, separate from the replay/live layout.
void* gdxsv_multi_pov_audio_map(size_t bytes);
