/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// gdxsv: added by the fork (docs/gdxsv/upstream.md)
// While the game runs with threaded rendering, the frames are rendered and presented on a render thread and the main
// thread, which owns the window, only handles its events. A Present that waits for the display then no longer
// delays the input events: the main thread reads them as they arrive, not after its next Present.
// The rest stays on the main thread as before: the GUI, renderer changes and the window. The render thread runs
// only while none of them is needed and stops for them.
#pragma once
#include <exception>
#include <functional>

namespace renderthread
{

// Main thread. Starts the render thread if it does not run.
void start();
// Main thread. True while it runs, and after it stopped by itself until stop() is called.
bool started();
// Main thread. True once it stopped by itself: the game stopped, the GUI opened, or an error.
bool finished();
// Main thread. Stops it and waits for it. Returns what it threw, if anything.
std::exception_ptr stop();
// Main thread, after stop(): true once if the render thread could not present (the context must be recreated).
bool takeReinit();

// True on the render thread.
bool current();

// Main thread: waits for a window event, at most timeoutMs. The render thread stopping by itself wakes it too.
void waitEvents(int timeoutMs);

// Main thread: the window size or the display scale changed. Applied now, or by the render thread before its next
// frame.
void displayChanged(bool scaleChanged);

// Render thread, or main thread while it runs: stops it; the main thread runs task when it has the context back, before it starts the render
// thread again. For what a graphics API allows only on the thread that created the device (DirectX 9 Reset).
void runOnMainThread(std::function<void()> task);

} // namespace renderthread
