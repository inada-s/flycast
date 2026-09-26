#pragma once
#include <SDL.h>
#include "types.h"

void input_sdl_init();
void input_sdl_handle();
void input_sdl_quit();
// nullptr before the window is created or in a headless run.
SDL_Window *sdl_get_window();
bool sdl_queue_open_file(const char *path);
void sdl_window_create();
void sdl_window_destroy();
// Preserve window preferences while a temporary layout is active. Clear after
// restoring the window; quitting earlier saves the preserved preferences.
void sdl_preserve_window_state(bool preserve);
bool sdl_recreate_window(u32 flags);
bool sdl_update_display_metrics(SDL_Window *window, u32 windowFlags);
void sdl_fix_steamdeck_dpi(SDL_Window *window);
