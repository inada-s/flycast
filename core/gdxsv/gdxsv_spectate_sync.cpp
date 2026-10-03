#include "gdxsv_spectate_sync.h"

#ifdef _WIN32
// MinGW's libstdc++ defines this already, so guard it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>

#include "log/LogManager.h"
#include "types.h"

namespace {
constexpr uint32_t kMagic = 0x47445853;	 // "GDXS"
constexpr int kMaxSlots = 8;

// A slot whose heartbeat is older than this is treated as dead, so a crashed
// or paused instance cannot hold the rest of the group at its last frame.
constexpr int64_t kSlotStaleUs = 1'000'000;

// Two input frames of tolerance, in position units. Enough to stop the pair
// thrashing against each other, far below what is visible side by side.
constexpr int32_t kSyncSlack = 2 * 1024;

int64_t NowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Shared memory every instance in the group maps by name. Zero-filled on
// creation on both platforms, so an uninitialised header reads as 0.
//
// Both platforms back this with a real file that outlives the group, which is
// why Join reclaims a slot by pid - a killed instance leaves its slot behind.
// Windows previously used an anonymous section named under "Local\\", but that
// namespace is scoped to the creating process's session; instances launched
// as separate child processes (e.g. one per spectator POV, spawned by a
// driver script or harness) can end up in different sessions and each then
// silently gets its own private, zero-filled section instead of sharing one -
// no error, just two peers that never see each other. A real file sidesteps
// session scoping entirely.
void* MapShared(const std::string& group, size_t size) {
#ifdef _WIN32
	char temp_dir[MAX_PATH];
	if (GetTempPathA(MAX_PATH, temp_dir) == 0) {
		WARN_LOG(COMMON, "spectate sync: GetTempPath failed");
		return nullptr;
	}
	const std::string path = std::string(temp_dir) + "gdxsv_spec_sync_" + group;
	HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
								OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		WARN_LOG(COMMON, "spectate sync: CreateFile failed %s", path.c_str());
		return nullptr;
	}
	HANDLE h = CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), nullptr);
	if (h == nullptr) {
		WARN_LOG(COMMON, "spectate sync: CreateFileMapping failed %s", path.c_str());
		CloseHandle(hFile);
		return nullptr;
	}
	void* m = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
	CloseHandle(h);		 // the mapped view keeps the section alive
	CloseHandle(hFile);	 // ditto
	if (m == nullptr) WARN_LOG(COMMON, "spectate sync: MapViewOfFile failed");
	return m;
#else
	const std::string path = "/tmp/gdxsv_spec_sync_" + group;
	const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
		WARN_LOG(COMMON, "spectate sync: open failed %s", path.c_str());
		return nullptr;
	}
	if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
		close(fd);
		return nullptr;
	}
	void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		WARN_LOG(COMMON, "spectate sync: mmap failed");
		return nullptr;
	}
	return m;
#endif
}

void UnmapShared(void* m, size_t size) {
#ifdef _WIN32
	(void)size;
	UnmapViewOfFile(m);
#else
	munmap(m, size);
#endif
}

int32_t CurrentPid() {
#ifdef _WIN32
	return static_cast<int32_t>(GetCurrentProcessId());
#else
	return static_cast<int32_t>(getpid());
#endif
}
}  // namespace

struct Slot {
	std::atomic<int64_t> heartbeat_us;
	std::atomic<int32_t> frame;
	std::atomic<int32_t> pid;
	// Seeking, taking over or draining a live buffer: not a position to hold
	// the group to. Published by the instance itself, since distance cannot
	// tell a seeking peer from a slow one.
	std::atomic<uint32_t> catching_up;
};

struct Header {
	std::atomic<uint32_t> magic;
	Slot slots[kMaxSlots];
};

GdxsvSpectateSync::~GdxsvSpectateSync() { Leave(); }

void GdxsvSpectateSync::Leave() {
	if (slot_ != nullptr) {
		slot_->heartbeat_us.store(0);  // release the slot for the next run
		slot_ = nullptr;
	}
	if (map_ != nullptr) {
		UnmapShared(map_, map_size_);
		map_ = nullptr;
	}
	group_.clear();
}

void GdxsvSpectateSync::Join(const std::string& group) {
	if (slot_ != nullptr && group == group_) return;  // Start() can run more than once; one slot each
	Leave();
	if (group.empty()) return;

	map_size_ = sizeof(Header);
	void* m = MapShared(group, map_size_);
	if (m == nullptr) return;
	map_ = m;

	Header* h = static_cast<Header*>(map_);
	h->magic.store(kMagic);

	// Reclaim our own slot first if we already have one - a killed instance
	// never runs its destructor, so its slot is left behind, and re-running
	// would otherwise leak a fresh slot on every restart until the table fills.
	const int64_t now = NowUs();
	const int32_t me = CurrentPid();
	for (int i = 0; i < kMaxSlots; ++i) {
		if (h->slots[i].pid.load() == me) {
			h->slots[i].heartbeat_us.store(now);
			h->slots[i].frame.store(0);
			slot_ = &h->slots[i];
			group_ = group;
			NOTICE_LOG(COMMON, "spectate sync: reclaimed slot %d in group %s", i, group.c_str());
			return;
		}
	}
	for (int i = 0; i < kMaxSlots; ++i) {
		const int64_t hb = h->slots[i].heartbeat_us.load();
		if (hb == 0 || kSlotStaleUs < now - hb) {
			h->slots[i].pid.store(me);
			h->slots[i].heartbeat_us.store(now);
			h->slots[i].frame.store(0);
			slot_ = &h->slots[i];
			group_ = group;
			NOTICE_LOG(COMMON, "spectate sync: joined group %s in slot %d", group.c_str(), i);
			return;
		}
	}
	WARN_LOG(COMMON, "spectate sync: no free slot in group %s", group.c_str());
}

void GdxsvSpectateSync::Publish(int32_t frame, bool catching_up) {
	if (slot_ == nullptr) return;
	slot_->frame.store(frame);
	slot_->catching_up.store(catching_up ? 1u : 0u);
	slot_->heartbeat_us.store(NowUs());
}

int32_t GdxsvSpectateSync::LeadOverSlowest(int32_t frame) const {
	if (slot_ == nullptr) return 0;

	const Header* h = static_cast<const Header*>(map_);
	const int64_t now = NowUs();
	int peers = 0;
	int32_t slowest = INT32_MAX;
	for (int i = 0; i < kMaxSlots; ++i) {
		const int64_t hb = h->slots[i].heartbeat_us.load(std::memory_order_acquire);
		if (hb == 0 || kSlotStaleUs < now - hb) continue;
		++peers;
		if (h->slots[i].catching_up.load(std::memory_order_acquire) != 0) continue;
		const int32_t f = h->slots[i].frame.load(std::memory_order_acquire);
		slowest = std::min(slowest, f);
	}
	if (peers <= 1 || slowest == INT32_MAX) return 0;
	return std::max(0, frame - slowest);
}

bool GdxsvSpectateSync::WaitForPeers(int32_t frame, int max_wait_ms) {
	if (slot_ == nullptr) return false;

	Publish(frame, false);

	Header* h = static_cast<Header*>(map_);
	const int64_t deadline = NowUs() + static_cast<int64_t>(max_wait_ms) * 1000;
	for (;;) {
		const int64_t now = NowUs();
		int peers = 0;
		int32_t slowest = INT32_MAX;
		for (int i = 0; i < kMaxSlots; ++i) {
			const int64_t hb = h->slots[i].heartbeat_us.load(std::memory_order_acquire);
			if (hb == 0 || kSlotStaleUs < now - hb) continue;
			++peers;
			if (h->slots[i].catching_up.load(std::memory_order_acquire) != 0) continue;
			const int32_t f = h->slots[i].frame.load(std::memory_order_acquire);
			slowest = std::min(slowest, f);
		}
		if (peers <= 1) return false;			 // alone
		if (slowest == INT32_MAX) return false;	 // everyone else is catching up

		// Wait only while somebody is meaningfully behind. Waiting for exact
		// equality deadlocks when instances arrive out of step, since each
		// blocks at its own frame waiting for the other. This converges
		// instead, because whoever is behind is never blocked. The slack
		// matters: without it an instance one position ahead blocks
		// immediately, and the pair alternates and ends up slower than either.
		if (frame <= slowest + kSyncSlack) return true;
		if (deadline < now) return false;

		// Spin on memory rather than yielding. yield() does not return promptly
		// under several competing processes - measured 10-13ms against a 2ms
		// deadline, since the deadline is only checked once it comes back, and
		// that was audible. A bounded spin costs at most the deadline.
		for (int i = 0; i < 64; ++i) {
#if defined(_WIN32)
			YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#elif defined(__aarch64__)
			asm volatile("yield");
#endif
		}
	}
}
