// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_input_hooks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "emulator.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_frame_trace.h"
#include "gdxsv_ggpo.h"
#include "input/gamepad_device.h"
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"
#endif

#ifdef USE_SDL
namespace {

// What the last poll saw, to report only changes
struct GdxsvJoystickPollState {
	u64 buttons = 0;  // up to 64 buttons
	u32 hats = 0;	  // 4 bits per hat, up to 8 hats
	std::vector<Sint16> axes;
};

struct GdxsvInputPoll {
	// The main thread and the emulator thread both poll
	std::mutex mutex;
	// By SDL joystick instance id; ids are never reused
	std::map<SDL_JoystickID, GdxsvJoystickPollState> joysticks;
};

GdxsvInputPoll input_poll;

void SetHatButtons(GamepadDevice& gamepad, u32 code, bool neg, bool pos) {
	gamepad.gamepad_btn_input(code, neg);
	gamepad.gamepad_btn_input(code + 1, pos);
}

void PollJoystick(SDL_Joystick* joystick, GamepadDevice& gamepad, GdxsvJoystickPollState& prev) {
	const int numButtons = std::min(SDL_JoystickNumButtons(joystick), 64);
	const int numAxes = SDL_JoystickNumAxes(joystick);
	const int numHats = std::min(SDL_JoystickNumHats(joystick), 8);

	if (prev.axes.size() != (size_t)numAxes) prev.axes.resize(numAxes, 0);

	u64 buttons = 0;
	for (int i = 0; i < numButtons; i++) {
		if (SDL_JoystickGetButton(joystick, i)) buttons |= 1ULL << i;
	}
	const u64 changed = buttons ^ prev.buttons;
	if (changed) {
		for (int i = 0; i < numButtons; i++) {
			if (changed & (1ULL << i)) gamepad.gamepad_btn_input(i, (buttons & (1ULL << i)) != 0);
		}
		prev.buttons = buttons;
	}

	for (int i = 0; i < numAxes; i++) {
		const Sint16 value = SDL_JoystickGetAxis(joystick, i);
		if (value != prev.axes[i]) {
			prev.axes[i] = value;
			gamepad.gamepad_axis_input(i, value);
		}
	}

	u32 hats = 0;
	for (int i = 0; i < numHats; i++) hats |= (SDL_JoystickGetHat(joystick, i) & 0xF) << (i * 4);
	const u32 hatChanged = hats ^ prev.hats;
	if (hatChanged) {
		for (int i = 0; i < numHats; i++) {
			if (!(hatChanged & (0xF << (i * 4)))) continue;
			// Same button codes as the SDL event path: (hat + 1) << 8, then up, down, left, right
			const Uint8 hat = (hats >> (i * 4)) & 0xF;
			const u32 code = (i + 1) << 8;
			SetHatButtons(gamepad, code + 0, (hat & SDL_HAT_UP) != 0, !(hat & SDL_HAT_UP) && (hat & SDL_HAT_DOWN));
			SetHatButtons(gamepad, code + 2, (hat & SDL_HAT_LEFT) != 0, !(hat & SDL_HAT_LEFT) && (hat & SDL_HAT_RIGHT));
		}
		prev.hats = hats;
	}
}

// Test only: an SDL virtual joystick that plays a script, one change per line:
// "<vblank> b|a|h <index> <value>" (button, axis, hat), vblanks counted from the first.
struct TestJoystickStep {
	u64 vblank;
	char kind;
	int index;
	int value;
};

struct TestJoystick {
	SDL_Joystick* joystick = nullptr;
	std::vector<TestJoystickStep> steps;
	size_t next = 0;
	u64 vblank = 0;
};

TestJoystick test_joystick;

void TestJoystickVBlank(Event, void*) {
	TestJoystick& t = test_joystick;
	t.vblank++;
	for (; t.next < t.steps.size() && t.steps[t.next].vblank <= t.vblank; t.next++) {
		const TestJoystickStep& s = t.steps[t.next];
		if (s.kind == 'b')
			SDL_JoystickSetVirtualButton(t.joystick, s.index, (Uint8)s.value);
		else if (s.kind == 'a')
			SDL_JoystickSetVirtualAxis(t.joystick, s.index, (Sint16)s.value);
		else
			SDL_JoystickSetVirtualHat(t.joystick, s.index, (Uint8)s.value);
		NOTICE_LOG(INPUT, "test joystick: vblank %llu %c %d %d", (unsigned long long)t.vblank, s.kind, s.index, s.value);
	}
}

}  // namespace
#endif

void gdxsv_input_attach_test_joystick() {
#ifdef USE_SDL
	const char* path = getenv("GDXSV_TEST_JOYSTICK");
	if (path == nullptr || test_joystick.joystick != nullptr) return;
	FILE* f = fopen(path, "r");
	if (f == nullptr) {
		WARN_LOG(INPUT, "test joystick: cannot open %s", path);
		return;
	}
	unsigned long long vblank;
	char kind;
	int index, value;
	while (fscanf(f, "%llu %c %d %d", &vblank, &kind, &index, &value) == 4)
		test_joystick.steps.push_back({vblank, kind, index, value});
	fclose(f);

	SDL_VirtualJoystickDesc desc{};
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.nhats = 1;
	desc.name = "gdxsv test joystick";
	const int device = SDL_JoystickAttachVirtualEx(&desc);
	test_joystick.joystick = device < 0 ? nullptr : SDL_JoystickOpen(device);
	if (test_joystick.joystick == nullptr) {
		WARN_LOG(INPUT, "test joystick: %s", SDL_GetError());
		return;
	}
	NOTICE_LOG(INPUT, "test joystick: attached, %d steps from %s", (int)test_joystick.steps.size(), path);
	EventManager::listen(Event::VBlank, TestJoystickVBlank);
#endif
}

void gdxsv_input_poll_joysticks() {
#ifdef USE_SDL
	std::lock_guard<std::mutex> lock(input_poll.mutex);
	SDL_JoystickUpdate();
	const int count = SDL_NumJoysticks();
	for (int i = 0; i < count; i++) {
		const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
		SDL_Joystick* joystick = SDL_JoystickFromInstanceID(id);
		std::shared_ptr<SDLGamepad> gamepad = SDLGamepad::GetSDLGamepad(id);
		if (joystick == nullptr || gamepad == nullptr) continue;
		PollJoystick(joystick, *gamepad, input_poll.joysticks[id]);
	}
#endif
}

void gdxsv_input_update_joysticks() {
	GamepadDevice::RampAnalog();
	gdxsv_input_poll_joysticks();
}

bool gdxsv_input_digital_axis(u32 state, u32 negDir, u32 posDir, s16& joystick) {
	if (!gdxsv_enabled()) return false;
	const u32 pressed = state & (negDir | posDir);
	if (pressed == 0 || pressed == (negDir | posDir))
		joystick = 0;
	else if (pressed == negDir)
		joystick = -32768;
	else
		joystick = 32767;
	return true;
}

bool gdxsv_input_press_sweep(int player) {
#ifdef USE_SDL
	static bool started = false;
	int who, startMs, holdMs, gapMin, gapMax;
	const char* env = getenv("GDXSV_PRESS_SWEEP");
	if (started || env == nullptr || sscanf(env, "%d,%d,%d,%d,%d", &who, &startMs, &holdMs, &gapMin, &gapMax) != 5 ||
		who != player || test_joystick.joystick == nullptr)
		return false;
	started = true;
	NOTICE_LOG(INPUT, "press sweep: player %d from %d ms, hold %d ms, gap %d-%d ms", who, startMs, holdMs, gapMin, gapMax);
	// Triggers rest at raw 0 = half pressed (A held via the gdxsv trigger mapping): release them.
	SDL_JoystickSetVirtualAxis(test_joystick.joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
	SDL_JoystickSetVirtualAxis(test_joystick.joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
	std::thread([=]() {
		std::mt19937 rng(540);
		std::uniform_int_distribution<int> gap(gapMin, gapMax);
		std::this_thread::sleep_for(std::chrono::milliseconds(startMs));
		// Random input until here gets the match into the battle; then the pad alone drives this peer.
		gdxsv_ggpo_random_input(false, 0, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		kcode[0] = ~0u;
		GDXSV_TRACE("sweep_start", 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		for (int n = 0;; n++) {
			// Alternate left / right so the unit stays in place.
			const int button = n % 2 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT;
			GDXSV_TRACE("vpress", button);
			SDL_JoystickSetVirtualButton(test_joystick.joystick, button, 1);
			std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
			GDXSV_TRACE("vrelease", button);
			SDL_JoystickSetVirtualButton(test_joystick.joystick, button, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(gap(rng)));
		}
	}).detach();
	return true;
#else
	return false;
#endif
}
