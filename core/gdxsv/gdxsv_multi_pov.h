#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 4-player replay: one replay watched from all four POVs at once, as four
// Flycast processes in a 2x2 grid ([1P, 2P] / [3P, 4P]).
//
// The 1P instance is the host: it reads the replay, spawns the three guests
// (os_RunInstance) and drives playback and the window layout. The guests get
// everything, the replay bytes included, through a file-backed shared memory
// session (same technique as GdxsvSpectateSync).

constexpr int kGdxsvMultiPovScreens = 4;

enum class GdxsvMultiPovRole {
	None,	// ordinary single-screen playback
	Host,	// 1P
	Guest,	// 2P-4P
};

struct GdxsvMultiPovRect {
	int32_t x = 0;
	int32_t y = 0;
	int32_t w = 0;
	int32_t h = 0;

	bool operator==(const GdxsvMultiPovRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
	bool operator!=(const GdxsvMultiPovRect& o) const { return !(*this == o); }
};

// The host's window layout. Guests place themselves relative to it.
struct GdxsvMultiPovHostWindow {
	GdxsvMultiPovRect rect;		 // the host's own quadrant, in desktop coords
	GdxsvMultiPovRect group;	 // the area the 2x2 grid covers
	bool maximized = false;		 // the grid tiles the work area
	bool fullscreen = false;	 // the grid tiles the whole display, borderless and on top (Alt+Enter)
	uint32_t generation = 0;	 // bumped on every change
};

// Playback state, published by the host every frame and followed by the guests.
struct GdxsvMultiPovPlayback {
	int64_t position = 0;		  // key_msg_count * kSyncSubFrames + subframe
	int32_t speed = 0;			  // replay speed index
	bool paused = false;
	bool menu_open = false;		  // the host's pause menu is up
	uint32_t seek_generation = 0; // bumped per seek
	int64_t seek_target = 0;	  // key message index the host seeked to
	int32_t seek_round = 0;		  // round the host jumped to with SetRound, 0 for other seeks

	// Replay options toggled on the host.
	bool show_ally_hp = false;
	bool key_display = false;
	bool skip_ms_selection = false;
	int32_t volume = 0;			  // aica.Volume
};

// ---- session ----------------------------------------------------------

std::string gdxsv_multi_pov_new_session_id();

// Host: creates the session and publishes the serialised replay for the guests.
bool gdxsv_multi_pov_host_create(const std::string& session_id, const std::vector<uint8_t>& replay);

// Guest: attaches to the host's session. `screen` is 1..3 (2P..4P).
bool gdxsv_multi_pov_guest_open(const std::string& session_id, int screen);

// Releases the mapping. The host also marks the session closed, which tells
// the guests to quit.
void gdxsv_multi_pov_close();

GdxsvMultiPovRole gdxsv_multi_pov_current_role();

// 0 for the host, 1..3 for the guests, -1 outside a session.
int gdxsv_multi_pov_screen_index();

// Guest: the replay bytes the host published. False on timeout.
bool gdxsv_multi_pov_fetch_replay(std::vector<uint8_t>& out, int timeout_ms);

// ---- start barrier ----------------------------------------------------
//
// The four screens reach playback at different times, so they line up once
// at the first StartMsg (key_msg_count 0). Both return false on timeout;
// playback goes on regardless.

bool gdxsv_multi_pov_guest_ready_and_wait(int timeout_ms);
bool gdxsv_multi_pov_host_wait_for_guests(int expected_guests, int timeout_ms);

// ---- per-frame publication --------------------------------------------

void gdxsv_multi_pov_publish_playback(const GdxsvMultiPovPlayback& state);
// False until the host has published at least once.
bool gdxsv_multi_pov_read_playback(GdxsvMultiPovPlayback& out);

void gdxsv_multi_pov_publish_host_window(const GdxsvMultiPovHostWindow& window);
bool gdxsv_multi_pov_read_host_window(GdxsvMultiPovHostWindow& out);

// Guest: the host closed the session or its process is gone.
bool gdxsv_multi_pov_host_gone();

// ---- grid geometry ----------------------------------------------------

// Splits `group` into out[0..3] = 1P top-left, 2P top-right, 3P bottom-left,
// 4P bottom-right. Odd pixels go to the right and bottom cells, so the four
// cover `group` exactly.
void gdxsv_multi_pov_compute_grid(const GdxsvMultiPovRect& group, GdxsvMultiPovRect out[kGdxsvMultiPovScreens]);

// ---- orchestration ----------------------------------------------------

// Consume an explicit command-line ReplayFourScreen request once. Saved
// emu.cfg values are ignored, and guests never become hosts.
bool gdxsv_multi_pov_take_four_screen_request();

// Host: reads `replay_source` (path or URL), publishes it and spawns the
// guests. `replay_out` holds the bytes for the host's own playback. False
// when the session cannot be set up or the battle is not four players; the
// caller then plays single-screen.
bool gdxsv_multi_pov_begin_host_session(const std::string& replay_source, std::vector<uint8_t>& replay_out);

// Guest: joins the session named on the command line and fetches the replay.
bool gdxsv_multi_pov_begin_guest_session(std::vector<uint8_t>& replay_out);

// The POV this guest plays, from the command line. -1 when not a guest.
int gdxsv_multi_pov_guest_pov();

// flycast.log for the host, flycast-<n>P.log for a guest: the four processes
// share one working directory.
std::string gdxsv_multi_pov_log_file_name();

// Blocks at the start barrier for the other screens. No-op outside a session.
void gdxsv_multi_pov_wait_at_start_barrier();
