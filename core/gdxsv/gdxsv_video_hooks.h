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
// While waiting for the next frame from the emulation thread: presents the last frame again if there is room; true
// if it did.
bool gdxsv_video_repeat_present_waiting();
#else
constexpr bool gdxsv_video_repeat_active = false;
inline bool gdxsv_video_repeat_present_waiting() { return false; }
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
