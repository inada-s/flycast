// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// UI/render thread only. Ordinary rendering pays only the flag check.
extern bool gdxsv_video_grid_active;
void gdxsv_video_viewport(int& x, int& y, int& width, int& height, bool bottom_origin = false);
void gdxsv_video_term();
// DXGI frame latency for the DX11 device (gdxsv:MaxFrameLatency); 0 keeps the driver default.
int gdxsv_video_max_frame_latency();

// gdxsv never duplicates frames for a high refresh rate display, neither by the swap interval nor by extra presents,
// whatever config::DupeFrames says: a refresh rate read at startup can be another display's, and the repeat present
// (gdxsv:RepeatPresent) shows each frame for the same number of vblanks instead.
constexpr bool gdxsv_video_dupe_frames = false;

// Repeat present (gdxsv_repeat_present.h). Render thread only.
#ifdef _WIN32
struct IDXGISwapChain;
// DX11 context created (its function presenting the last frame again) or destroyed (nullptr).
void gdxsv_video_dx11_attach(void (*present_again)());
// After each DX11 present: whether the compositor composes the window.
void gdxsv_video_dx11_presented(IDXGISwapChain* swapchain);
extern bool gdxsv_video_repeat_active;
// While the render thread waits for the next frame from the emulation thread, before that frame starts rendering:
// -1 after presenting the last frame again; 0 after sleeping 1 ms while the limiter's thread runs (a rollback, the next
// frame), so that the queue is only polled and the repeats start as soon as that thread waits; timeout otherwise (ms).
// A 1 ms condition variable wait can last a timer tick, hence the own sleep.
int gdxsv_video_repeat_wait_ms(int timeout);
#else
constexpr bool gdxsv_video_repeat_active = false;
inline int gdxsv_video_repeat_wait_ms(int timeout) { return timeout; }
#endif

// A background replay worker still renders, but never shows its SDL window.
bool gdxsv_video_hidden_window();
// Optional stable name for video routing; empty keeps the ordinary sender name.
std::string gdxsv_video_sender_name();

#if defined(_WIN32) && defined(VIDEO_ROUTING) && defined(USE_VULKAN) && !defined(LIBRETRO)
#include <cstdint>
#include <set>
#include <vector>
// Windows Vulkan hosts import textures received from DirectX guests.
void gdxsv_video_vulkan_extensions(uint32_t device_version, const std::set<std::string>& supported, std::vector<const char*>& enabled);
#endif
