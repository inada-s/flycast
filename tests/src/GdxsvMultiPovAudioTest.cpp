// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"

#if GTEST_HAS_DEATH_TEST && !defined(_WIN32) && !defined(LIBRETRO)
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>

#include "audio/audiostream.h"
#include "cfg/option.h"
#include "gdxsv/gdxsv_multi_pov.h"
#include "gdxsv/gdxsv_multi_pov_audio.h"

namespace {
struct GdxsvAudioTestSample {
	int16_t left, right;
};

class GdxsvAudioTestOutput : public AudioBackend {
   public:
	GdxsvAudioTestOutput() : AudioBackend("gdxsv-test-output", "Test PCM capture") {}
	bool init() override { return true; }
	u32 push(const void* data, u32 count, bool wait) override {
		EXPECT_FALSE(wait);
		const auto* samples = static_cast<const GdxsvAudioTestSample*>(data);
		pcm.insert(pcm.end(), samples, samples + count);
		return 1;
	}
	std::vector<GdxsvAudioTestSample> pcm;
};

struct GdxsvAudioTestCommand {
	int input = 0, speed = 0;
	bool silent = false, request = false, misaligned = false;
	int marker_frame = -1;
	bool paused = false, tone = false;
	int samples = 735;
	int marker_sample = -1;
	bool timeline_markers = false;
	bool constant = false;
};

int GdxsvAudioTestTone(int sample, int screen) {
	return static_cast<int>(8000 * std::sin(2 * 3.141592653589793 * (223 + 71 * screen) * sample / 44100));
}

bool GdxsvAudioTestMarker(int sample) {
	// Each frame puts its marker at a different sample. Even a whole-frame
	// shift of otherwise identical music must fail the alignment check.
	return sample % 735 == 300 + (sample / 735 * 53) % 400;
}

class GdxsvAudioTestSession {
   public:
	~GdxsvAudioTestSession() {
		for (auto& child : children_) {
			if (child.fd >= 0) close(child.fd);
			if (child.pid > 0) {
				kill(child.pid, SIGKILL);
				int status;
				while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
				}
			}
		}
		TermAudio();
		gdxsv_multi_pov_close();
	}

	bool Start() {
		config::setTransient("gdxsv", "headless", "no");
		config::setTransient("gdxsv", "MultiPovAudioSync", "yes");
		config::setTransient("gdxsv", "MultiPovMixVolume", "100");
		config::setTransient("gdxsv", "AudioRateControl", "no");
		config::AudioBackend.override(output.slug);
		config::AudioVolume.logarithmic_volume_scale = .5f;
		settings.gdxsv.replayModeActive = true;
		const auto session = gdxsv_multi_pov_new_session_id();
		// Fork before creating the host mapping: a child must join as a guest,
		// without inheriting a host session that its open would close.
		for (int i = 0; i < 3; ++i) {
			int sockets[2];
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
			const pid_t pid = fork();
			if (pid < 0) {
				close(sockets[0]);
				close(sockets[1]);
				return false;
			}
			if (pid == 0) {
				close(sockets[0]);
				for (int j = 0; j < i; ++j) close(children_[j].fd);
				Guest(session, i + 1, sockets[1]);
			}
			close(sockets[1]);
			children_[i] = {pid, sockets[0]};
		}
		// The session transports opaque replay bytes; these tests supply PCM
		// directly and do not load a game or parse a replay.
		if (!gdxsv_multi_pov_host_create(session, {0}) || !gdxsv_multi_pov_audio_enabled()) return false;
		gdxsv_multi_pov_audio_start();
		InitAudio();
		for (int i = 0; i < 3; ++i)
			if (!Send(i, {-1})) return false;
		return true;
	}

	bool Send(int guest, const GdxsvAudioTestCommand& command) {
		const int fd = children_[guest].fd;
		if (write(fd, &command, sizeof(command)) != sizeof(command)) {
			ADD_FAILURE() << "guest command write failed";
			return false;
		}
		char reply;
		if (!Read(fd, &reply, sizeof(reply))) {
			ADD_FAILURE() << "guest did not complete its frame";
			return false;
		}
		return reply != 0;
	}

	bool Frame(const GdxsvAudioTestCommand& command, int screen = 0) {
		if (command.input < 0) {
			gdxsv_multi_pov_audio_reset();
			frames_ = 0;
			samples_ = 0;
			return true;
		}
		if (command.request) return gdxsv_multi_pov_audio_input_ready(command.input, 1, 0, true);
		if (!command.silent) {
			for (int j = 0; j < command.samples; ++j) {
				const int point = j - (command.misaligned && screen == 1 ? 8 : 0);
				int amplitude = point == 64 ? 8000 : (point == 128 + screen * 32 ? 4000 : 0);
				if (command.marker_frame >= 0) amplitude = point == 64 && frames_ == command.marker_frame ? 8000 : 0;
				if (command.tone) amplitude = GdxsvAudioTestTone(samples_ + j, screen);
				if (command.marker_sample >= 0) amplitude = samples_ + j == command.marker_sample ? 8000 : 0;
				if (command.timeline_markers) amplitude = GdxsvAudioTestMarker(samples_ + j) ? 8000 : 0;
				if (command.constant) amplitude = 8000;
				WriteSample(-amplitude, amplitude);
			}
			samples_ += command.samples;
		}
		gdxsv_multi_pov_audio_frame(command.input, 1, 0, command.speed, command.silent, false, command.paused);
		++frames_;
		return true;
	}

	GdxsvAudioTestOutput output;

   private:
	static bool Read(int fd, void* data, size_t size) {
		auto* bytes = static_cast<char*>(data);
		while (size != 0) {
			pollfd poll_fd{fd, POLLIN, 0};
			if (poll(&poll_fd, 1, 5000) <= 0) return false;
			const auto n = read(fd, bytes, size);
			if (n <= 0) return false;
			bytes += n;
			size -= n;
		}
		return true;
	}

	[[noreturn]] void Guest(const std::string& session, int screen, int fd) {
		GdxsvAudioTestCommand command;
		if (!Read(fd, &command, sizeof(command)) || !gdxsv_multi_pov_guest_open(session, screen) || !gdxsv_multi_pov_audio_enabled())
			_exit(1);
		gdxsv_multi_pov_audio_start();
		InitAudio();
		do {
			const char reply = Frame(command, screen);
			if (!output.pcm.empty() || write(fd, &reply, sizeof(reply)) != sizeof(reply)) _exit(1);
		} while (Read(fd, &command, sizeof(command)));
		TermAudio();
		gdxsv_multi_pov_close();
		_exit(::testing::Test::HasFailure() ? 1 : 0);
	}

	struct Child {
		pid_t pid = -1;
		int fd = -1;
	};
	std::array<Child, 3> children_;
	int frames_ = 0, samples_ = 0;
};

bool Aligned(const std::vector<GdxsvAudioTestSample>& pcm) {
	if (pcm.size() <= 110 * 735) return false;
	for (size_t j = 0; j < pcm.size(); ++j) {
		const int offset = j == 0 ? -1 : (j - 1) % 735;
		const int expected = offset == 64 ? 4000 : (offset >= 128 && offset <= 224 && (offset - 128) % 32 == 0 ? 500 : 0);
		if (pcm[j].left != expected || pcm[j].right != -expected) return false;
	}
	return true;
}

void Alignment(GdxsvAudioTestSession& session, int speed, bool misaligned) {
	const int stride = speed + 1;
	for (int n = 0; n < 120 * stride; ++n) {
		for (int i = 0; i < 3; ++i) {
			const int input = n - i * stride;
			if (input >= 0) ASSERT_TRUE(session.Send(i, {input, speed, false, false, misaligned}));
		}
		session.Frame({n, speed, n % stride != 0});
	}
	EXPECT_GT(session.output.pcm.size(), 110u * 735);
	EXPECT_EQ(Aligned(session.output.pcm), !misaligned);
}

void QueueRecovery(GdxsvAudioTestSession& session) {
	// An ahead guest produces seconds of audio while the host consumes none,
	// then rewinds behind those queued positions.
	for (int n = 1000; n < 1240; ++n) ASSERT_TRUE(session.Send(0, {n}));
	for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {-1}));
	session.Frame({-1});
	for (int n = 0; n < 120; ++n) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {n}));
		session.Frame({n});
	}
	// The full queue first discards its pre-rewind packets. The remaining
	// output must recover all four copies of the shared impulse.
	EXPECT_GE(std::count_if(session.output.pcm.begin(), session.output.pcm.end(),
							[](const auto& sample) { return sample.left == 4000 && sample.right == -4000; }),
			  110);
}

void PausedRestore(GdxsvAudioTestSession& session, bool initially_aligned) {
	// Sound is tied to time since each restore, not time since the host's
	// restore. Late guests must not replay that marker against newer audio.
	for (int n = 0; n < 1300; ++n) {
		if (n == 1100)
			for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {-1}));
		session.Frame({500, 0, false, false, false, 50, true});
		for (int i = 0; i < 3; ++i)
			if ((initially_aligned || n >= 13 + 7 * i) && !(n >= 1100 && n < 1113 + 7 * i))
				ASSERT_TRUE(session.Send(i, {500, 0, false, false, false, 50, true}));
	}
	ASSERT_GT(session.output.pcm.size(), 1250u * 735);
	int markers = 0;
	for (size_t j = 0; j < session.output.pcm.size(); ++j) {
		const auto& sample = session.output.pcm[j];
		if (sample.left == 0 && sample.right == 0) continue;
		++markers;
		ASSERT_EQ(j, 50u * 735 + 65) << "a late paused restore replayed an earlier sound";
		EXPECT_EQ(sample.left, initially_aligned ? 4000 : 1000);
		EXPECT_EQ(sample.right, -sample.left);
	}
	EXPECT_EQ(markers, 1);
}

void PausedPacketSizes(GdxsvAudioTestSession& session) {
	// Runs can stop between vblanks or produce no samples. The same audio
	// arrives split across packets on one guest and combined on another.
	for (int n = 0; n < 800; ++n) {
		const bool paused = n >= 32 && n < 700;
		for (int screen = 1; screen < 4; ++screen) {
			GdxsvAudioTestCommand command{
				paused ? 32 + std::max(0, (n - 32 - 1 - (n / 12 + screen) % 2) / 2) : n, 0, false, false, false, -1, paused, true};
			if (paused && screen == 1) {
				for (int samples : {200, 0, 535}) {
					command.samples = samples;
					ASSERT_TRUE(session.Send(screen - 1, command));
				}
			} else {
				if (paused && screen == 2) command.samples = n % 2 == 0 ? 1470 : 0;
				if (paused && screen == 3) command.samples = n % 2 == 0 ? 512 : 958;
				ASSERT_TRUE(session.Send(screen - 1, command));
			}
		}
		GdxsvAudioTestCommand command{paused ? 32 + (n - 32) / 2 : n, 0, false, false, false, -1, paused, true};
		if (paused && n % 3 == 0) {
			command.samples = 0;
			session.Frame(command);
			command.samples = 735;
		}
		session.Frame(command);
	}
	ASSERT_GT(session.output.pcm.size(), 780u * 735);
	for (size_t j = 1; j < session.output.pcm.size(); ++j) {
		int sum = 0;
		for (int screen = 0; screen < 4; ++screen) sum += GdxsvAudioTestTone(j - 1, screen) / 2;
		ASSERT_EQ(session.output.pcm[j].left, sum / 4) << "packet boundaries interrupted audio at sample " << j;
		ASSERT_EQ(session.output.pcm[j].right, -sum / 4);
	}
}

void PausedStall(GdxsvAudioTestSession& session, int duration, bool host_stall, bool jitter = false) {
	const bool stays_aligned = host_stall && duration <= 10;
	for (int n = 0; n < 700; ++n) {
		const bool stalled = n >= 100 && n < 100 + duration;
		GdxsvAudioTestCommand command{500, 0, false, false, false, -1, true};
		command.timeline_markers = stays_aligned;
		command.constant = !stays_aligned;
		const size_t before = session.output.pcm.size();
		if (!stalled || !host_stall) session.Frame(command);
		for (int i = 0; i < 3; ++i) {
			command.samples = jitter && i == 0 && n >= 100 + duration ? (n % 2 == 0 ? 736 : 734) : 735;
			if (!stalled || host_stall || i != 0) ASSERT_TRUE(session.Send(i, command));
		}
		if (stays_aligned || n >= 100 + duration + 12) {
			for (size_t j = before; j < session.output.pcm.size(); ++j) {
				const int expected = !stays_aligned || (j != 0 && GdxsvAudioTestMarker(j - 1)) ? 4000 : 0;
				ASSERT_EQ(session.output.pcm[j].left, expected) << "lost guest audio or shifted a buffered marker at sample " << j;
				ASSERT_EQ(session.output.pcm[j].right, -expected);
			}
		}
	}
	EXPECT_GT(session.output.pcm.size(), static_cast<size_t>(650 - duration) * 735);
}

void PausedResumePartialPacket(GdxsvAudioTestSession& session, bool suffix) {
	const int marker = 32 * 735 + (suffix ? 900 : 64);
	for (int n = 0; n < 80; ++n) {
		const GdxsvAudioTestCommand silent{n, 0, false, false, false, 10000, n == 32};
		for (int i = 0; i < 3; ++i) {
			if (i == 0 && n == 33) continue;
			auto command = silent;
			if (i == 0) {
				// The guest packet for the next input spans two host runs. Its
				// prefix is consumed while paused, before labels realign on resume.
				command.marker_sample = marker;
				if (n == 32) {
					command.input = 33;
					command.samples = 1100;
				}
			}
			ASSERT_TRUE(session.Send(i, command));
		}
		session.Frame(silent);
	}
	int markers = 0;
	for (size_t j = 0; j < session.output.pcm.size(); ++j) {
		if (session.output.pcm[j].left == 0 && session.output.pcm[j].right == 0) continue;
		++markers;
		EXPECT_EQ(j, static_cast<size_t>(marker + 1)) << "resume repeated or stretched a partial packet";
	}
	if (suffix)
		EXPECT_LE(markers, 1);
	else
		EXPECT_EQ(markers, 1);
}

void GuestAudioSplices(GdxsvAudioTestSession& session) {
	for (int n = 0; n < 180; ++n) {
		// Paused stepping keeps each tone running; resume changes the relative
		// input alignment, and a stalled guest then loses and recovers packets.
		const bool paused = n < 60;
		for (int i = 0; i < 3; ++i) {
			if (n >= 100 && n < 105 && i == 0) continue;
			const int input = 500 + std::max(0, n - 60 - i - 1);
			ASSERT_TRUE(session.Send(i, {input, 0, false, false, false, -1, paused, true}));
		}
		session.Frame({paused ? 500 : 500 + n - 60, 0, false, false, false, -1, paused, true});
	}
	ASSERT_GT(session.output.pcm.size(), 170u * 735);
	int max_jump = 0, peak = 0;
	for (size_t j = 1; j < session.output.pcm.size(); ++j) {
		max_jump = std::max(max_jump, std::abs(session.output.pcm[j].left - session.output.pcm[j - 1].left));
		peak = std::max(peak, std::abs(int(session.output.pcm[j].left)));
	}
	EXPECT_LT(max_jump, 250) << "an audio gap or changed alignment introduced a click";
	EXPECT_GT(peak, 3000) << "guest audio was lost";
}

void BriefingRequests(GdxsvAudioTestSession& session) {
	for (int n = 0; n < 10; ++n) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
		session.Frame({500});
	}
	session.output.pcm.clear();
	ASSERT_FALSE(session.Send(1, {500, 0, false, true}));
	ASSERT_FALSE(session.Send(2, {500, 0, false, true}));
	ASSERT_FALSE(gdxsv_multi_pov_audio_input_ready(500, 1, 0, true));
	for (int n = 0; n < 13; ++n) {
		session.Frame({500});
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
	}
	EXPECT_GT(session.output.pcm.size(), 10u * 735);
	ASSERT_TRUE(session.Send(0, {500, 0, false, true}));
	EXPECT_TRUE(gdxsv_multi_pov_audio_input_ready(500, 1, 0, true));
	for (int i = 1; i < 3; ++i) EXPECT_TRUE(session.Send(i, {500, 0, false, true}));

	// Keep the peers alive without submitting the next request.
	const auto started = std::chrono::steady_clock::now();
	ASSERT_FALSE(gdxsv_multi_pov_audio_input_ready(501, 1, 0, true));
	while (!gdxsv_multi_pov_audio_input_ready(501, 1, 0, true)) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
		session.Frame({500});
		usleep(1000);
		ASSERT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
	}
	EXPECT_FALSE(gdxsv_multi_pov_audio_input_pending());
}

void PlaybackSnapshots(GdxsvAudioTestSession&) {
	std::atomic<bool> done{false};
	std::thread writer([&] {
		for (uint32_t i = 1; i <= 200000; ++i) {
			GdxsvMultiPovPlayback state;
			state.seek_generation = i;
			state.seek_target = i * 11;
			state.position = static_cast<int64_t>(i) * 1024;
			state.paused = i % 2 != 0;
			state.menu_open = i % 3 != 0;
			gdxsv_multi_pov_publish_playback(state);
		}
		done.store(true, std::memory_order_release);
	});
	int snapshots = 0, mixed = 0;
	do {
		GdxsvMultiPovPlayback state;
		if (!gdxsv_multi_pov_read_playback(state)) continue;
		++snapshots;
		const uint32_t generation = state.seek_generation;
		if (state.seek_target != generation * 11 || state.position != static_cast<int64_t>(generation) * 1024 ||
			state.paused != (generation % 2 != 0) || state.menu_open != (generation % 3 != 0))
			++mixed;
	} while (!done.load(std::memory_order_acquire));
	writer.join();
	EXPECT_GT(snapshots, 0);
	EXPECT_EQ(mixed, 0) << "a reader observed fields from different host updates";
}

// Isolate backend registration and transient configuration from other tests.
// The three workers then use the real shared session and linked audio code.
template <typename Check>
[[noreturn]] void RunAudioCheck(Check check) {
	alarm(20);
	{
		GdxsvAudioTestSession session;
		if (session.Start())
			check(session);
		else
			ADD_FAILURE() << "could not create the audio session";
	}
	const auto* result = ::testing::UnitTest::GetInstance()->current_test_info()->result();
	for (int i = 0; i < result->total_part_count(); ++i) {
		const auto& part = result->GetTestPartResult(i);
		if (part.failed()) std::fprintf(stderr, "%s:%d: %s\n", part.file_name(), part.line_number(), part.summary());
	}
	std::fflush(stdout);
	_exit(::testing::Test::HasFailure() ? 1 : 0);
}
}  // namespace

class GdxsvMultiPovAudioTest : public ::testing::Test {
	void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(GdxsvMultiPovAudioTest, AlignsAt1x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 0, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, AlignsAt2x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 1, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, AlignsAt3x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 2, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RejectsDelayedCopy) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 0, true); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RecoversAfterAnAheadGuestRewinds) {
	ASSERT_EXIT(RunAudioCheck(QueueRecovery), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, PlaysPausedRestoreMarkerOnce) {
	for (bool aligned : {false, true})
		ASSERT_EXIT(RunAudioCheck([=](auto& session) { PausedRestore(session, aligned); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, KeepsTonesContinuousAcrossUnevenPausedStepsAndPackets) {
	ASSERT_EXIT(RunAudioCheck(PausedPacketSizes), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RecoversFromPausedStallsAndQueueOverflow) {
	for (bool host_stall : {false, true})
		for (int frames : {2, 3, 5, 30, 60})
			ASSERT_EXIT(RunAudioCheck([=](auto& session) { PausedStall(session, frames, host_stall); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, KeepsShortHostFreezesAligned) {
	for (int frames : {5, 10})
		ASSERT_EXIT(RunAudioCheck([=](auto& session) { PausedStall(session, frames, true); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RecoversAcrossStallsAndShortPackets) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { PausedStall(session, 3, false, true); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, DoesNotRepeatOrStretchPartialGuestPacketsOnResume) {
	for (bool suffix : {false, true})
		ASSERT_EXIT(RunAudioCheck([=](auto& session) { PausedResumePartialPacket(session, suffix); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, SmoothsGuestGapsAndResume) {
	ASSERT_EXIT(RunAudioCheck(GuestAudioSplices), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, BriefingWaitKeepsAudioPlayingAndExpires) {
	ASSERT_EXIT(RunAudioCheck(BriefingRequests), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, ReadsCoherentPlaybackSnapshotsWhileHostPublishes) {
	ASSERT_EXIT(RunAudioCheck(PlaybackSnapshots), ::testing::ExitedWithCode(0), "");
}
#else
TEST(GdxsvMultiPovAudioTest, RequiresFork) { GTEST_SKIP() << "Cross-process audio checks require POSIX fork and googletest death tests."; }
#endif
