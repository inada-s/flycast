// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Input hooks for core/input and core/sdl.

// Reads the state of every open SDL joystick and feeds the changes to its
// gamepad device, without the SDL event queue (option JoystickPolling). Safe
// to call from the emulator thread.
void gdxsv_input_poll_joysticks();

// gdxsv_input_poll_joysticks() plus the digital-to-analog ramp: the input
// update of the emulator thread when rendering is threaded.
void gdxsv_input_update_joysticks();

// Test only: with env GDXSV_TEST_JOYSTICK=<script>, attaches an SDL virtual
// gamepad that plays the script on vblanks (see gdxsv_input.cpp).
void gdxsv_input_attach_test_joystick();

// Investigation only: with env GDXSV_PRESS_SWEEP=<player 0-3>,<start ms>,<hold ms>,<gap min ms>,<gap max ms>
// and the test joystick attached, a host thread presses dpad left / right at random times and traces them.
// Random input of that peer stays on until the start, so the match reaches the battle.
// Returns true if it started for this player.
bool gdxsv_input_press_sweep(int player);

// A digital direction pair mapped to an analog axis. With gdxsv enabled the
// axis goes to full deflection at once and opposite directions cancel out;
// returns true then and the caller skips its ramp.
bool gdxsv_input_digital_axis(u32 state, u32 negDir, u32 posDir, s16& joystick);
