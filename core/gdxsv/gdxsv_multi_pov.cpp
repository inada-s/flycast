#include "gdxsv_multi_pov.h"

#ifdef _WIN32
// MinGW's libstdc++ defines this already, so guard it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "gdxsv.pb.h"
#include "log/LogManager.h"
#include "types.h"

constexpr uint32_t kMagic = 0x4D505634;	 // "MPV4"
constexpr uint32_t kVersion = 4;

// The replay payload follows the header at this offset.
constexpr size_t kPayloadOffset = 4096;

// Sanity cap on the replay size read from a header another process wrote.
constexpr uint64_t kMaxReplayBytes = 256ull * 1024 * 1024;

// Live session: room for the battle code in the header.
constexpr size_t kLiveCodeWords = 8;
constexpr size_t kMaxLiveCodeLen = kLiveCodeWords * sizeof(uint64_t);

// Live session: what the host receives from LBS, relayed to the guests. The
// bootstrap header (users, rule, patches), the round state and close as one
// serialised BattleLogFile, and the inputs as they arrive.
constexpr size_t kFeedHeaderOffset = kPayloadOffset;
constexpr size_t kFeedHeaderCap = 256 * 1024;
constexpr size_t kFeedRoundsOffset = kFeedHeaderOffset + kFeedHeaderCap;
constexpr size_t kFeedRoundsCap = 64 * 1024;
constexpr size_t kFeedInputsOffset = kFeedRoundsOffset + kFeedRoundsCap;
constexpr int32_t kMaxFeedInputs = 1 << 20;  // hours of battle
constexpr size_t kLiveSessionSize = kFeedInputsOffset + kMaxFeedInputs * sizeof(uint64_t);

// "Gone" means the process is gone: a heartbeat cannot tell a busy host (one
// loading a game holds its UI thread for seconds) from a dead one.
static bool ProcessAlive(int32_t pid) {
	if (pid <= 0) return false;
#ifdef _WIN32
	HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (proc == nullptr) return false;
	const bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
	CloseHandle(proc);
	return alive;
#else
	// EPERM means it exists and is not ours; ESRCH means it is gone.
	return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

static int32_t CurrentPid() {
#ifdef _WIN32
	return static_cast<int32_t>(GetCurrentProcessId());
#else
	return static_cast<int32_t>(getpid());
#endif
}

// Shared by the four processes. Every field has a single writer, so atomics
// are enough.
struct GdxsvMultiPovHeader {
	std::atomic<uint32_t> magic;
	std::atomic<uint32_t> version;
	std::atomic<uint64_t> total_size;
	std::atomic<uint64_t> replay_size;
	std::atomic<uint32_t> replay_ready;

	std::atomic<int32_t> host_pid;
	std::atomic<uint32_t> host_closed;

	// Start barrier: each guest sets its bit, the host raises `go`.
	std::atomic<uint32_t> ready_mask;
	std::atomic<uint32_t> go;

	std::atomic<int32_t> guest_pid[kGdxsvMultiPovScreens];

	// Playback, written by the host every frame.
	std::atomic<int64_t> position;
	std::atomic<int32_t> speed;
	std::atomic<uint32_t> paused;
	std::atomic<uint32_t> menu_open;
	std::atomic<uint32_t> seek_generation;
	std::atomic<int64_t> seek_target;
	std::atomic<int32_t> seek_round;
	std::atomic<uint32_t> show_ally_hp;
	std::atomic<uint32_t> key_display;
	std::atomic<uint32_t> skip_ms_selection;
	std::atomic<int32_t> volume;
	std::atomic<uint32_t> live_following;
	// The guests start before the host publishes; until then the fields
	// above are zero and must not be followed.
	std::atomic<uint32_t> playback_published;

	// Window layout, written by the host whenever it moves or resizes.
	std::atomic<int32_t> win_x, win_y, win_w, win_h;
	std::atomic<int32_t> grp_x, grp_y, grp_w, grp_h;
	std::atomic<uint32_t> maximized;
	std::atomic<uint32_t> fullscreen;
	std::atomic<uint32_t> win_generation;
	std::atomic<int64_t> guest_window[kGdxsvMultiPovScreens];

	// Live session: the battle every screen watches. live_seq is a seqlock,
	// odd while the host rewrites the code; seq / 2 is the battle generation.
	std::atomic<uint32_t> live;
	std::atomic<uint32_t> live_seq;
	std::atomic<uint64_t> live_code[kLiveCodeWords];

	// Live feed, written by the host. feed_generation names the battle
	// generation the feed holds; feed_rounds_seq is a seqlock over the rounds blob.
	std::atomic<uint32_t> feed_generation;
	std::atomic<uint32_t> feed_header_size;
	std::atomic<uint32_t> feed_header_ready;
	std::atomic<uint32_t> feed_rounds_seq;
	std::atomic<uint32_t> feed_rounds_size;
	std::atomic<int32_t> feed_inputs;
	std::atomic<uint32_t> feed_backlog;
};

static_assert(sizeof(GdxsvMultiPovHeader) <= kPayloadOffset, "header must fit before the payload");

static std::string SessionPath(const std::string& session_id) {
#ifdef _WIN32
	char temp_dir[MAX_PATH];
	if (GetTempPathA(MAX_PATH, temp_dir) == 0) return {};
	return std::string(temp_dir) + "gdxsv_multi_pov_" + session_id;
#else
	return "/tmp/gdxsv_multi_pov_" + session_id;
#endif
}

// Maps the session file. `create` lays it out at `size`; otherwise it must
// already be at least that big (reading past the end is a SIGBUS on POSIX).
static void* MapSession(const std::string& path, size_t size, bool create) {
#ifdef _WIN32
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
							  create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		WARN_LOG(COMMON, "multi-pov: CreateFile failed %s (%lu)", path.c_str(), GetLastError());
		return nullptr;
	}
	if (!create) {
		LARGE_INTEGER on_disk{};
		if (!GetFileSizeEx(file, &on_disk) || static_cast<uint64_t>(on_disk.QuadPart) < size) {
			CloseHandle(file);
			return nullptr;
		}
	}
	HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<uint64_t>(size) >> 32),
										static_cast<DWORD>(size & 0xffffffffu), nullptr);
	if (mapping == nullptr) {
		WARN_LOG(COMMON, "multi-pov: CreateFileMapping failed %s (%lu)", path.c_str(), GetLastError());
		CloseHandle(file);
		return nullptr;
	}
	void* m = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
	CloseHandle(mapping);  // the view keeps the section alive
	CloseHandle(file);
	if (m == nullptr) WARN_LOG(COMMON, "multi-pov: MapViewOfFile failed (%lu)", GetLastError());
	return m;
#else
	const int fd = open(path.c_str(), create ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR, 0666);
	if (fd < 0) {
		WARN_LOG(COMMON, "multi-pov: open failed %s", path.c_str());
		return nullptr;
	}
	if (create) {
		if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
			WARN_LOG(COMMON, "multi-pov: ftruncate failed %s", path.c_str());
			close(fd);
			return nullptr;
		}
	} else {
		struct stat st {};
		if (fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < size) {
			close(fd);
			return nullptr;
		}
	}
	void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		WARN_LOG(COMMON, "multi-pov: mmap failed %s", path.c_str());
		return nullptr;
	}
	return m;
#endif
}

static void UnmapSession(void* m, size_t size) {
	if (m == nullptr) return;
#ifdef _WIN32
	(void)size;
	UnmapViewOfFile(m);
#else
	munmap(m, size);
#endif
}

static uint64_t SessionFileSize(const std::string& path) {
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA attr{};
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr)) return 0;
	return (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
#else
	struct stat st {};
	if (stat(path.c_str(), &st) != 0) return 0;
	return static_cast<uint64_t>(st.st_size);
#endif
}

// This process's session: at most one per process.
struct GdxsvMultiPovSession {
	GdxsvMultiPovRole role = GdxsvMultiPovRole::None;
	int screen = -1;
	void* map = nullptr;
	size_t map_size = 0;
	std::string path;

	GdxsvMultiPovHeader* header() const { return static_cast<GdxsvMultiPovHeader*>(map); }
	uint8_t* payload() const { return static_cast<uint8_t*>(map) + kPayloadOffset; }
	uint8_t* at(size_t offset) const { return static_cast<uint8_t*>(map) + offset; }

	// Live feed. Host: what is already published. Guest: the generation it
	// plays and the rounds blob it last applied.
	int32_t feed_published_inputs = 0;
	std::string feed_published_rounds;
	uint32_t feed_generation = 0;
	uint32_t feed_applied_rounds_seq = 0;
};

static GdxsvMultiPovSession g_session;

static bool HostAlive(const GdxsvMultiPovHeader* h) {
	if (h->host_closed.load(std::memory_order_acquire) != 0) return false;
	const int32_t pid = h->host_pid.load(std::memory_order_acquire);
	return pid == 0 || ProcessAlive(pid);  // 0: still being created
}

std::string gdxsv_multi_pov_new_session_id() {
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return std::to_string(CurrentPid()) + "_" + std::to_string(now);
}

// Lays out a fresh session of `size` bytes with this process as its host.
static bool CreateHostSession(const std::string& session_id, size_t size) {
	gdxsv_multi_pov_close();

	const std::string path = SessionPath(session_id);
	if (path.empty()) return false;

	void* m = MapSession(path, size, true);
	if (m == nullptr) return false;

	g_session = {};
	g_session.role = GdxsvMultiPovRole::Host;
	g_session.screen = 0;
	g_session.map = m;
	g_session.map_size = size;
	g_session.path = path;

	GdxsvMultiPovHeader* h = g_session.header();
	std::memset(m, 0, kPayloadOffset);
	h->version.store(kVersion, std::memory_order_relaxed);
	h->total_size.store(size, std::memory_order_relaxed);
	h->host_pid.store(CurrentPid(), std::memory_order_relaxed);
	return true;
}

bool gdxsv_multi_pov_host_create(const std::string& session_id, const std::vector<uint8_t>& replay) {
	if (session_id.empty()) return false;
	if (replay.empty() || kMaxReplayBytes < replay.size()) {
		WARN_LOG(COMMON, "multi-pov: refusing to publish a %zu byte replay", replay.size());
		return false;
	}
	if (!CreateHostSession(session_id, kPayloadOffset + replay.size())) return false;

	GdxsvMultiPovHeader* h = g_session.header();
	std::memcpy(g_session.payload(), replay.data(), replay.size());
	h->replay_size.store(replay.size(), std::memory_order_relaxed);
	h->replay_ready.store(1, std::memory_order_release);
	h->magic.store(kMagic, std::memory_order_release);

	NOTICE_LOG(COMMON, "multi-pov: host published %zu replay bytes to session %s", replay.size(), session_id.c_str());
	return true;
}

static void WriteLiveBattle(GdxsvMultiPovHeader* h, const std::string& battle_code) {
	uint64_t words[kLiveCodeWords] = {};
	std::memcpy(words, battle_code.data(), std::min(battle_code.size(), kMaxLiveCodeLen));
	const uint32_t seq = h->live_seq.load(std::memory_order_relaxed);
	h->live_seq.store(seq + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	for (size_t i = 0; i < kLiveCodeWords; ++i) h->live_code[i].store(words[i], std::memory_order_relaxed);
	h->live_seq.store(seq + 2, std::memory_order_release);
}

bool gdxsv_multi_pov_host_create_live(const std::string& session_id, const std::string& battle_code) {
	if (session_id.empty() || battle_code.empty() || kMaxLiveCodeLen < battle_code.size()) return false;
	if (!CreateHostSession(session_id, kLiveSessionSize)) return false;

	GdxsvMultiPovHeader* h = g_session.header();
	h->live.store(1, std::memory_order_relaxed);
	WriteLiveBattle(h, battle_code);
	h->feed_generation.store(h->live_seq.load(std::memory_order_relaxed) / 2, std::memory_order_relaxed);
	h->magic.store(kMagic, std::memory_order_release);

	NOTICE_LOG(COMMON, "multi-pov: host opened live session %s for %s", session_id.c_str(), battle_code.c_str());
	return true;
}

bool gdxsv_multi_pov_host_publish_live_battle(const std::string& battle_code) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host || h->live.load(std::memory_order_relaxed) == 0)
		return false;
	if (battle_code.empty() || kMaxLiveCodeLen < battle_code.size()) return false;

	// A new battle starts over: a fresh start barrier, and no playback to
	// follow until the host publishes its own.
	h->playback_published.store(0, std::memory_order_relaxed);
	h->ready_mask.store(0, std::memory_order_relaxed);
	h->go.store(0, std::memory_order_relaxed);
	// The feed starts over too, before the guests can see the new battle.
	h->feed_header_ready.store(0, std::memory_order_relaxed);
	h->feed_inputs.store(0, std::memory_order_relaxed);
	h->feed_backlog.store(1, std::memory_order_relaxed);
	g_session.feed_published_inputs = 0;
	g_session.feed_published_rounds.clear();
	WriteLiveBattle(h, battle_code);
	h->feed_generation.store(h->live_seq.load(std::memory_order_relaxed) / 2, std::memory_order_release);
	NOTICE_LOG(COMMON, "multi-pov: live session moves on to %s, generation %u", battle_code.c_str(),
			   h->live_seq.load(std::memory_order_relaxed) / 2);
	return true;
}

bool gdxsv_multi_pov_is_live() {
	const GdxsvMultiPovHeader* h = g_session.header();
	return h != nullptr && h->live.load(std::memory_order_relaxed) != 0;
}

bool gdxsv_multi_pov_read_live_battle(std::string& battle_code, uint32_t& generation) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || h->live.load(std::memory_order_relaxed) == 0) return false;

	const uint32_t before = h->live_seq.load(std::memory_order_acquire);
	if (before & 1) return false;
	uint64_t words[kLiveCodeWords];
	for (size_t i = 0; i < kLiveCodeWords; ++i) words[i] = h->live_code[i].load(std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_acquire);
	if (h->live_seq.load(std::memory_order_relaxed) != before) return false;

	const char* chars = reinterpret_cast<const char*>(words);
	battle_code.assign(chars, strnlen(chars, kMaxLiveCodeLen));
	generation = before / 2;
	return !battle_code.empty();
}

bool gdxsv_multi_pov_guest_open(const std::string& session_id, int screen) {
	if (session_id.empty()) return false;
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) {
		WARN_LOG(COMMON, "multi-pov: bad guest screen index %d", screen);
		return false;
	}
	gdxsv_multi_pov_close();

	const std::string path = SessionPath(session_id);
	if (path.empty()) return false;

	// The host lays the whole file out before spawning anyone.
	const uint64_t on_disk = SessionFileSize(path);
	if (on_disk < kPayloadOffset) {
		WARN_LOG(COMMON, "multi-pov: session %s is not ready (%llu bytes)", session_id.c_str(), (unsigned long long)on_disk);
		return false;
	}

	void* m = MapSession(path, static_cast<size_t>(on_disk), false);
	if (m == nullptr) return false;

	GdxsvMultiPovHeader* h = static_cast<GdxsvMultiPovHeader*>(m);
	if (h->magic.load(std::memory_order_acquire) != kMagic || h->version.load(std::memory_order_acquire) != kVersion) {
		WARN_LOG(COMMON, "multi-pov: session %s has a bad header", session_id.c_str());
		UnmapSession(m, static_cast<size_t>(on_disk));
		return false;
	}

	g_session = {};
	g_session.role = GdxsvMultiPovRole::Guest;
	g_session.screen = screen;
	g_session.map = m;
	g_session.map_size = static_cast<size_t>(on_disk);
	g_session.path = path;
	g_session.header()->guest_pid[screen].store(CurrentPid(), std::memory_order_release);

	NOTICE_LOG(COMMON, "multi-pov: guest %dP joined session %s", screen + 1, session_id.c_str());
	return true;
}

void gdxsv_multi_pov_close() {
	if (g_session.map == nullptr) {
		g_session.role = GdxsvMultiPovRole::None;
		g_session.screen = -1;
		return;
	}
	GdxsvMultiPovHeader* h = g_session.header();
	if (g_session.role == GdxsvMultiPovRole::Host) {
		h->host_closed.store(1, std::memory_order_release);
	} else if (0 <= g_session.screen && g_session.screen < kGdxsvMultiPovScreens) {
		h->guest_pid[g_session.screen].store(0, std::memory_order_release);
	}
	UnmapSession(g_session.map, g_session.map_size);
	g_session.map = nullptr;
	g_session.map_size = 0;
	g_session.role = GdxsvMultiPovRole::None;
	g_session.screen = -1;
	g_session.path.clear();
}

GdxsvMultiPovRole gdxsv_multi_pov_current_role() { return g_session.role; }

int gdxsv_multi_pov_screen_index() { return g_session.screen; }

bool gdxsv_multi_pov_fetch_replay(std::vector<uint8_t>& out, int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (h->replay_ready.load(std::memory_order_acquire) == 0) {
		if (!HostAlive(h)) {
			WARN_LOG(COMMON, "multi-pov: host went away before publishing the replay");
			return false;
		}
		if (deadline <= std::chrono::steady_clock::now()) {
			WARN_LOG(COMMON, "multi-pov: timed out waiting for the replay payload");
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	const uint64_t size = h->replay_size.load(std::memory_order_acquire);
	if (size == 0 || kPayloadOffset + size > g_session.map_size) {
		WARN_LOG(COMMON, "multi-pov: replay size %llu does not fit the session", (unsigned long long)size);
		return false;
	}
	out.assign(g_session.payload(), g_session.payload() + size);
	return true;
}

void gdxsv_multi_pov_guest_ready() {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest) return;
	h->ready_mask.fetch_or(1u << g_session.screen, std::memory_order_acq_rel);
}

bool gdxsv_multi_pov_guest_ready_and_wait(int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest) return false;

	gdxsv_multi_pov_guest_ready();

	// Live: the host may give up on this battle and move on to another.
	const uint32_t live_seq = h->live_seq.load(std::memory_order_acquire);
	const auto waiting_since = std::chrono::steady_clock::now();
	const auto deadline = waiting_since + std::chrono::milliseconds(timeout_ms);
	while (h->go.load(std::memory_order_acquire) == 0) {
		if (!HostAlive(h)) {
			WARN_LOG(COMMON, "multi-pov: host went away before the start signal");
			return false;
		}
		if (h->live_seq.load(std::memory_order_acquire) != live_seq) {
			WARN_LOG(COMMON, "multi-pov: host moved on before the start signal");
			return false;
		}
		if (deadline <= std::chrono::steady_clock::now()) {
			WARN_LOG(COMMON, "multi-pov: no start signal within %d ms; starting anyway", timeout_ms);
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	NOTICE_LOG(COMMON, "multi-pov: start barrier released after %lld ms",
			   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waiting_since).count());
	return true;
}

static int ReadyGuestCount() {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return 0;
	const uint32_t mask = h->ready_mask.load(std::memory_order_acquire);
	int n = 0;
	for (int i = 1; i < kGdxsvMultiPovScreens; ++i)
		if (mask & (1u << i)) ++n;
	return n;
}

bool gdxsv_multi_pov_host_wait_for_guests(int expected_guests, int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return false;
	if (expected_guests <= 0) {
		h->go.store(1, std::memory_order_release);
		return true;
	}

	const auto waiting_since = std::chrono::steady_clock::now();
	const auto deadline = waiting_since + std::chrono::milliseconds(timeout_ms);
	bool all_in = false;
	while (std::chrono::steady_clock::now() < deadline) {
		if (expected_guests <= ReadyGuestCount()) {
			all_in = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (!all_in)
		WARN_LOG(COMMON, "multi-pov: only %d of %d guests reported ready; starting without the rest", ReadyGuestCount(), expected_guests);

	h->go.store(1, std::memory_order_release);
	NOTICE_LOG(COMMON, "multi-pov: start barrier released %d screens after %lld ms", ReadyGuestCount() + 1,
			   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waiting_since).count());
	return all_in;
}

void gdxsv_multi_pov_publish_playback(const GdxsvMultiPovPlayback& state) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return;
	h->position.store(state.position, std::memory_order_relaxed);
	h->speed.store(state.speed, std::memory_order_relaxed);
	h->paused.store(state.paused ? 1u : 0u, std::memory_order_relaxed);
	h->menu_open.store(state.menu_open ? 1u : 0u, std::memory_order_relaxed);
	h->seek_target.store(state.seek_target, std::memory_order_relaxed);
	h->seek_round.store(state.seek_round, std::memory_order_relaxed);
	h->show_ally_hp.store(state.show_ally_hp ? 1u : 0u, std::memory_order_relaxed);
	h->key_display.store(state.key_display ? 1u : 0u, std::memory_order_relaxed);
	h->skip_ms_selection.store(state.skip_ms_selection ? 1u : 0u, std::memory_order_relaxed);
	h->volume.store(state.volume, std::memory_order_relaxed);
	h->live_following.store(state.live_following ? 1u : 0u, std::memory_order_relaxed);
	// Last, so a guest that sees the generation sees the target with it.
	h->seek_generation.store(state.seek_generation, std::memory_order_release);
	h->playback_published.store(1, std::memory_order_release);
}

bool gdxsv_multi_pov_read_playback(GdxsvMultiPovPlayback& out) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	if (h->playback_published.load(std::memory_order_acquire) == 0) return false;
	out.seek_generation = h->seek_generation.load(std::memory_order_acquire);
	out.seek_target = h->seek_target.load(std::memory_order_relaxed);
	out.seek_round = h->seek_round.load(std::memory_order_relaxed);
	out.position = h->position.load(std::memory_order_relaxed);
	out.speed = h->speed.load(std::memory_order_relaxed);
	out.paused = h->paused.load(std::memory_order_relaxed) != 0;
	out.menu_open = h->menu_open.load(std::memory_order_relaxed) != 0;
	out.show_ally_hp = h->show_ally_hp.load(std::memory_order_relaxed) != 0;
	out.key_display = h->key_display.load(std::memory_order_relaxed) != 0;
	out.skip_ms_selection = h->skip_ms_selection.load(std::memory_order_relaxed) != 0;
	out.volume = h->volume.load(std::memory_order_relaxed);
	out.live_following = h->live_following.load(std::memory_order_relaxed) != 0;
	return true;
}

void gdxsv_multi_pov_publish_host_window(const GdxsvMultiPovHostWindow& window) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return;
	h->win_x.store(window.rect.x, std::memory_order_relaxed);
	h->win_y.store(window.rect.y, std::memory_order_relaxed);
	h->win_w.store(window.rect.w, std::memory_order_relaxed);
	h->win_h.store(window.rect.h, std::memory_order_relaxed);
	h->grp_x.store(window.group.x, std::memory_order_relaxed);
	h->grp_y.store(window.group.y, std::memory_order_relaxed);
	h->grp_w.store(window.group.w, std::memory_order_relaxed);
	h->grp_h.store(window.group.h, std::memory_order_relaxed);
	h->maximized.store(window.maximized ? 1u : 0u, std::memory_order_relaxed);
	h->fullscreen.store(window.fullscreen ? 1u : 0u, std::memory_order_relaxed);
	h->win_generation.store(window.generation, std::memory_order_release);
}

bool gdxsv_multi_pov_read_host_window(GdxsvMultiPovHostWindow& out) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	out.generation = h->win_generation.load(std::memory_order_acquire);
	if (out.generation == 0) return false;
	out.rect.x = h->win_x.load(std::memory_order_relaxed);
	out.rect.y = h->win_y.load(std::memory_order_relaxed);
	out.rect.w = h->win_w.load(std::memory_order_relaxed);
	out.rect.h = h->win_h.load(std::memory_order_relaxed);
	out.group.x = h->grp_x.load(std::memory_order_relaxed);
	out.group.y = h->grp_y.load(std::memory_order_relaxed);
	out.group.w = h->grp_w.load(std::memory_order_relaxed);
	out.group.h = h->grp_h.load(std::memory_order_relaxed);
	out.maximized = h->maximized.load(std::memory_order_relaxed) != 0;
	out.fullscreen = h->fullscreen.load(std::memory_order_relaxed) != 0;
	return true;
}

void gdxsv_multi_pov_publish_guest_window(int64_t handle) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest) return;
	h->guest_window[g_session.screen].store(handle, std::memory_order_release);
}

void gdxsv_multi_pov_read_guest_windows(int64_t out[kGdxsvMultiPovScreens]) {
	const GdxsvMultiPovHeader* h = g_session.header();
	for (int i = 0; i < kGdxsvMultiPovScreens; ++i)
		out[i] = h == nullptr ? 0 : h->guest_window[i].load(std::memory_order_acquire);
}

bool gdxsv_multi_pov_host_gone() {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	return !HostAlive(h);
}

// ---- live feed ----------------------------------------------------------

static bool HostingLiveFeed() {
	const GdxsvMultiPovHeader* h = g_session.header();
	return h != nullptr && g_session.role == GdxsvMultiPovRole::Host && h->live.load(std::memory_order_relaxed) != 0 &&
		   kLiveSessionSize <= g_session.map_size;
}

static uint32_t CurrentLiveGeneration(const GdxsvMultiPovHeader* h) {
	return h->live_seq.load(std::memory_order_acquire) / 2;
}

bool gdxsv_multi_pov_feed_publish_header(const proto::BattleLogFile& header) {
	if (!HostingLiveFeed()) return false;
	GdxsvMultiPovHeader* h = g_session.header();
	const std::string bytes = header.SerializeAsString();
	if (kFeedHeaderCap < bytes.size()) {
		WARN_LOG(COMMON, "multi-pov: live header of %zu bytes does not fit the session", bytes.size());
		return false;
	}
	std::memcpy(g_session.at(kFeedHeaderOffset), bytes.data(), bytes.size());
	h->feed_header_size.store(static_cast<uint32_t>(bytes.size()), std::memory_order_relaxed);
	h->feed_header_ready.store(1, std::memory_order_release);
	return true;
}

void gdxsv_multi_pov_feed_publish(const proto::BattleLogFile& log, bool backlog) {
	if (!HostingLiveFeed()) return;
	GdxsvMultiPovHeader* h = g_session.header();

	// Inputs before the rounds blob: a guest that sees the close has every input.
	const int32_t n = std::min(log.inputs_size(), kMaxFeedInputs);
	if (g_session.feed_published_inputs < n) {
		uint64_t* dst = reinterpret_cast<uint64_t*>(g_session.at(kFeedInputsOffset));
		for (int32_t i = g_session.feed_published_inputs; i < n; ++i) dst[i] = log.inputs(i);
		h->feed_inputs.store(n, std::memory_order_release);
		g_session.feed_published_inputs = n;
	}

	proto::BattleLogFile rounds;
	rounds.mutable_start_msg_indexes()->CopyFrom(log.start_msg_indexes());
	rounds.mutable_start_msg_randoms()->CopyFrom(log.start_msg_randoms());
	rounds.mutable_round_data()->CopyFrom(log.round_data());
	rounds.set_close_reason(log.close_reason());
	rounds.set_disconnect_user_index(log.disconnect_user_index());
	std::string bytes = rounds.SerializeAsString();
	if (bytes != g_session.feed_published_rounds && bytes.size() <= kFeedRoundsCap) {
		const uint32_t seq = h->feed_rounds_seq.load(std::memory_order_relaxed);
		h->feed_rounds_seq.store(seq + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(g_session.at(kFeedRoundsOffset), bytes.data(), bytes.size());
		h->feed_rounds_size.store(static_cast<uint32_t>(bytes.size()), std::memory_order_relaxed);
		h->feed_rounds_seq.store(seq + 2, std::memory_order_release);
		g_session.feed_published_rounds = std::move(bytes);
	}

	h->feed_backlog.store(backlog ? 1u : 0u, std::memory_order_relaxed);
}

bool gdxsv_multi_pov_feed_wait_header(proto::BattleLogFile* out, int timeout_ms) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest || h->live.load(std::memory_order_relaxed) == 0 ||
		g_session.map_size < kLiveSessionSize)
		return false;

	const uint32_t generation = CurrentLiveGeneration(h);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (int i = 0;; ++i) {
		if (CurrentLiveGeneration(h) != generation) {
			WARN_LOG(COMMON, "multi-pov: host moved on before this battle's header");
			return false;
		}
		if (h->feed_header_ready.load(std::memory_order_acquire) != 0 &&
			h->feed_generation.load(std::memory_order_acquire) == generation) {
			const uint32_t size = h->feed_header_size.load(std::memory_order_relaxed);
			if (kFeedHeaderCap < size || !out->ParseFromArray(g_session.at(kFeedHeaderOffset), static_cast<int>(size))) {
				WARN_LOG(COMMON, "multi-pov: bad live header from the host");
				return false;
			}
			g_session.feed_generation = generation;
			g_session.feed_applied_rounds_seq = 0;
			return true;
		}
		if (deadline <= std::chrono::steady_clock::now()) {
			WARN_LOG(COMMON, "multi-pov: no live header from the host within %d ms", timeout_ms);
			return false;
		}
		if (i % 64 == 0 && !HostAlive(h)) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

bool gdxsv_multi_pov_feed_drain(proto::BattleLogFile* log, bool* backlog_pending) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest || g_session.map_size < kLiveSessionSize) return false;
	const auto same_battle = [h]() {
		return CurrentLiveGeneration(h) == g_session.feed_generation &&
			   h->feed_generation.load(std::memory_order_acquire) == g_session.feed_generation;
	};
	// Another battle's feed: the tick moves this screen on to it.
	if (!same_battle()) return false;

	// The rounds blob first: once it carries the close, the inputs read below are complete.
	bool have_rounds = false;
	proto::BattleLogFile rounds;
	const uint32_t seq = h->feed_rounds_seq.load(std::memory_order_acquire);
	if ((seq & 1) == 0 && seq != g_session.feed_applied_rounds_seq) {
		const uint32_t size = h->feed_rounds_size.load(std::memory_order_relaxed);
		if (size <= kFeedRoundsCap) {
			std::string bytes(reinterpret_cast<const char*>(g_session.at(kFeedRoundsOffset)), size);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (h->feed_rounds_seq.load(std::memory_order_relaxed) == seq && rounds.ParseFromString(bytes)) have_rounds = true;
		}
	}

	std::vector<uint64_t> inputs;
	const int32_t have = log->inputs_size();
	const int32_t n = std::min(h->feed_inputs.load(std::memory_order_acquire), kMaxFeedInputs);
	if (have < n) {
		const uint64_t* src = reinterpret_cast<const uint64_t*>(g_session.at(kFeedInputsOffset));
		inputs.assign(src + have, src + n);
	}

	// The host may have moved on while this was being read.
	if (!same_battle()) return false;

	for (const uint64_t input : inputs) log->add_inputs(input);
	if (have_rounds) {
		g_session.feed_applied_rounds_seq = seq;
		log->mutable_start_msg_indexes()->CopyFrom(rounds.start_msg_indexes());
		log->mutable_start_msg_randoms()->CopyFrom(rounds.start_msg_randoms());
		log->mutable_round_data()->CopyFrom(rounds.round_data());
		if (!rounds.close_reason().empty() && log->close_reason().empty()) {
			log->set_close_reason(rounds.close_reason());
			log->set_disconnect_user_index(rounds.disconnect_user_index());
		}
	}
	if (backlog_pending != nullptr) *backlog_pending = h->feed_backlog.load(std::memory_order_relaxed) != 0;
	return !inputs.empty() || have_rounds;
}
