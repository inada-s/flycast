// Backend for targets without desktop windows (Android, Switch, UWP): the grid
// reports itself unavailable.
#include "gdxsv_multi_pov_window.h"

bool gdxsv_multi_pov_window_available() { return false; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() { return {}; }

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState&) {}

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() { return {}; }

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect&) {}

bool gdxsv_multi_pov_window_is_maximized() { return false; }

void gdxsv_multi_pov_window_unmaximize() {}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() { return {}; }

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() { return {}; }

void gdxsv_multi_pov_window_set_topmost(bool) {}

void gdxsv_multi_pov_window_set_borderless(bool) {}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() { return {}; }
