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
//
// Live Spectate works the same way: only the host talks to LBS, and it relays
// what it receives (the bootstrap header, inputs, round state and close)
// through the session as it arrives. The session outlives a battle: the host
// publishes the next one and the guests follow.

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

	// Live: the host is chasing the live edge. Each screen then chases the
	// edge of the relayed feed on its own, and the guests ignore the host's
	// seeks, pause and speed.
	bool live_following = false;
};

// ---- session ----------------------------------------------------------

std::string gdxsv_multi_pov_new_session_id();

// Host: creates the session and publishes the serialised replay for the guests.
bool gdxsv_multi_pov_host_create(const std::string& session_id, const std::vector<uint8_t>& replay);

// Host: creates a live session on `battle_code`. The guests play what the
// host relays through the feed below.
bool gdxsv_multi_pov_host_create_live(const std::string& session_id, const std::string& battle_code);

// Host: the live session moves on to another battle. Resets the start barrier.
bool gdxsv_multi_pov_host_publish_live_battle(const std::string& battle_code);

bool gdxsv_multi_pov_is_live();

// Live session: the battle the host is on, and a generation bumped per
// battle. False outside a live session, or while the host is rewriting it.
bool gdxsv_multi_pov_read_live_battle(std::string& battle_code, uint32_t& generation);

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
// Guest: counts as ready without waiting, for a screen that sits a battle out.
void gdxsv_multi_pov_guest_ready();
bool gdxsv_multi_pov_host_wait_for_guests(int expected_guests, int timeout_ms);

// ---- per-frame publication --------------------------------------------

void gdxsv_multi_pov_publish_playback(const GdxsvMultiPovPlayback& state);
// False until the host has published at least once.
bool gdxsv_multi_pov_read_playback(GdxsvMultiPovPlayback& out);

void gdxsv_multi_pov_publish_host_window(const GdxsvMultiPovHostWindow& window);
bool gdxsv_multi_pov_read_host_window(GdxsvMultiPovHostWindow& out);

// Guest: its native window, for the host to keep above its own. Host: all of
// them by screen index, 0 where not published.
void gdxsv_multi_pov_publish_guest_window(int64_t handle);
void gdxsv_multi_pov_read_guest_windows(int64_t out[kGdxsvMultiPovScreens]);

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

// Host: opens a live session on `battle_code` and spawns the guests. The
// caller checks that the battle has four players.
bool gdxsv_multi_pov_begin_live_host_session(const std::string& battle_code);

// What a guest plays: the host's replay, or a live battle.
struct GdxsvMultiPovGuestStart {
	std::vector<uint8_t> replay;
	std::string live_battle_code;  // empty in a replay session
	uint32_t live_generation = 0;
};

// Guest: joins the session named on the command line (once: a live guest
// stays in it across battles) and reads what to play.
bool gdxsv_multi_pov_begin_guest_session(GdxsvMultiPovGuestStart& out);

// The POV this guest plays, from the command line. -1 when not a guest.
int gdxsv_multi_pov_guest_pov();

// flycast.log for the host, flycast-<n>P.log for a guest: the four processes
// share one working directory.
std::string gdxsv_multi_pov_log_file_name();

// Blocks at the start barrier for the other screens. No-op outside a session.
void gdxsv_multi_pov_wait_at_start_barrier();
// A guest that cannot play this battle releases the host from waiting for it.
void gdxsv_multi_pov_skip_start_barrier();

// ---- live feed ----------------------------------------------------------

namespace proto { class BattleLogFile; }

// Host: the battle's bootstrap header, as received from LBS (no inputs yet).
bool gdxsv_multi_pov_feed_publish_header(const proto::BattleLogFile& header);

// Host: whatever is new in `log`, the live log as received from LBS.
// `backlog`: the host is still downloading the battle so far.
void gdxsv_multi_pov_feed_publish(const proto::BattleLogFile& log, bool backlog);

// Guest: waits for the current battle's header. False on timeout, or when
// the host moves on or goes away first.
bool gdxsv_multi_pov_feed_wait_header(proto::BattleLogFile* out, int timeout_ms);

// Guest: folds what the host relayed into `log`, as
// GdxsvSpectatorDownlink::DrainInto does. True when anything was folded in.
bool gdxsv_multi_pov_feed_drain(proto::BattleLogFile* log, bool* backlog_pending);
