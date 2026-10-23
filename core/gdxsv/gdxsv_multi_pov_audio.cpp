// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_audio.h"

#ifndef LIBRETRO

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "audio/audiostream.h"
#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_multi_pov.h"

namespace {
constexpr uint32_t kPackets = 16;					// three displayed frames at 3x, plus the guests' allowed lead
constexpr uint32_t kSamples = 16384;				// a run can span several vblanks during loading
constexpr uint32_t kAudioMagic = 0x41505635;		// "APV5": local PCM layout, not the replay protocol
constexpr uint32_t kSpliceSamples = 220;			// 5ms at 44.1kHz to join discontinuous guest audio
constexpr uint32_t kPausedBufferSamples = 3 * 735;	// minimum samples in three 735/736-sample frames
// Keep short freezes buffered, with two nominal packets of room before full.
constexpr uint32_t kPausedMaxBufferedSamples = 14 * 736;

uint32_t gdxsv_audio_now_ms() {
	return static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct GdxsvAudioSample {
	int16_t left, right;
};
static_assert(sizeof(GdxsvAudioSample) == 4);

struct GdxsvAudioPacket {
	uint64_t epoch = 0;
	uint64_t serial = 0;
	uint64_t sample_position = 0;
	uint32_t round = 0;
	int input = 0;
	uint32_t frame = 0;
	uint32_t reset = 0;
	uint32_t count = 0;
	GdxsvAudioSample samples[kSamples];

	int Compare(const GdxsvAudioPacket& other) const {
		if (epoch != other.epoch) return epoch < other.epoch ? -1 : 1;
		if (round != other.round) return round < other.round ? -1 : 1;
		if (input != other.input) return input < other.input ? -1 : 1;
		if (frame != other.frame) return frame < other.frame ? -1 : 1;
		return 0;
	}
	void Copy(const GdxsvAudioPacket& other) {
		epoch = other.epoch;
		serial = other.serial;
		sample_position = other.sample_position;
		round = other.round;
		input = other.input;
		frame = other.frame;
		reset = other.reset;
		count = other.count;
		std::memcpy(samples, other.samples, count * sizeof(*samples));
	}
};

// An exact replay-position match establishes the relative audio clocks. Keep
// that alignment through paused steps: input advances asynchronously, but the
// AICA keeps running on every screen even while the game waits for input.
struct GdxsvAudioCursor {
	uint64_t epoch = 0, host_serial = 0, guest_serial = 0;
	uint64_t host_sample = 0, guest_sample = 0;
	uint32_t round = 0, reset = 0;
	bool valid = false, buffering = false;

	bool Matches(const GdxsvAudioPacket& wanted, uint32_t producer_reset) const {
		return valid && epoch == wanted.epoch && round == wanted.round && reset == producer_reset;
	}
	uint64_t Next(const GdxsvAudioPacket& wanted) const { return guest_serial + (wanted.serial - host_serial); }
	uint64_t NextSample(const GdxsvAudioPacket& wanted) const { return guest_sample + (wanted.sample_position - host_sample); }
	void Set(const GdxsvAudioPacket& wanted, const GdxsvAudioPacket& guest) {
		epoch = wanted.epoch;
		round = wanted.round;
		reset = guest.reset;
		host_serial = wanted.serial;
		guest_serial = guest.serial;
		host_sample = wanted.sample_position + wanted.count;
		guest_sample = guest.sample_position + guest.count;
		valid = true;
		buffering = false;
	}
};

// Single producer and single consumer. A full queue drops new PCM, never
// holds emulation. A producer reset invalidates even an ahead-of-host head.
struct GdxsvAudioQueue {
	std::atomic<uint32_t> read{0}, write{0}, reset{0};
	std::atomic<uint32_t> heartbeat{0};
	GdxsvAudioPacket packets[kPackets];

	void Push(const GdxsvAudioPacket& packet) {
		heartbeat.store(gdxsv_audio_now_ms(), std::memory_order_relaxed);
		const uint32_t w = write.load(std::memory_order_relaxed);
		if (w - read.load(std::memory_order_acquire) >= kPackets) return;
		auto& slot = packets[w % kPackets];
		slot.Copy(packet);
		slot.reset = reset.load(std::memory_order_relaxed);
		write.store(w + 1, std::memory_order_release);
	}
	bool Take(const GdxsvAudioPacket& wanted, GdxsvAudioPacket& result, GdxsvAudioCursor* cursor, bool paused = true) {
		if (paused && cursor != nullptr && cursor->Matches(wanted, reset.load(std::memory_order_acquire)))
			return TakeSamples(wanted, result, *cursor);
		uint32_t r = read.load(std::memory_order_relaxed);
		const uint32_t w = write.load(std::memory_order_acquire);
		if (w - r > kPackets) {
			read.store(w, std::memory_order_release);
			return false;
		}
		while (r != w) {
			const auto& packet = packets[r % kPackets];
			const bool valid = packet.reset == reset.load(std::memory_order_acquire) && packet.count <= kSamples;
			const int order = valid ? packet.Compare(wanted) : -1;
			if (order > 0) return false;
			bool found = valid && order == 0;
			if (found && cursor != nullptr && cursor->Matches(wanted, packet.reset) && cursor->guest_sample > packet.sample_position) {
				// Paused mixing may leave a packet suffix at the queue head.
				// Treat it as a break: replaying the prefix repeats sound, and
				// stretching just the suffix over a whole frame changes pitch.
				found = false;
			}
			if (found) result.Copy(packet);
			found = found && packet.reset == reset.load(std::memory_order_acquire);
			read.store(++r, std::memory_order_release);
			if (found) return true;
		}
		return false;
	}
	bool TakeSamples(const GdxsvAudioPacket& wanted, GdxsvAudioPacket& result, GdxsvAudioCursor& cursor) {
		uint32_t r = read.load(std::memory_order_relaxed);
		const uint32_t w = write.load(std::memory_order_acquire);
		if (w - r > kPackets) {
			read.store(w, std::memory_order_release);
			return false;
		}
		if (wanted.count == 0) return false;
		// Inspect before consuming. Only the newest contiguous range can
		// recover from a producer stall or packets dropped by a full queue.
		// Keep the read index fixed until copying finishes, so the producer
		// cannot overwrite any of the packets inspected here.
		uint64_t begin = 0, end = 0;
		uint32_t first = w, discarded = r;
		for (uint32_t i = r; i != w; ++i) {
			const auto& packet = packets[i % kPackets];
			if (packet.reset != cursor.reset || packet.count > kSamples) {
				if (discarded == i) ++discarded;
				continue;
			}
			if (packet.epoch != wanted.epoch || packet.round != wanted.round) continue;
			const uint64_t packet_end = packet.sample_position + packet.count;
			if (packet.count == 0 || packet_end <= cursor.guest_sample) {
				if (discarded == i) ++discarded;
				continue;
			}
			const uint64_t packet_begin = std::max(packet.sample_position, cursor.guest_sample);
			if (first == w || packet_begin != end) {
				first = i;
				begin = packet_begin;
			}
			end = packet_end;
		}
		if (reset.load(std::memory_order_acquire) != cursor.reset) return false;
		uint64_t position = cursor.NextSample(wanted);
		if (first == w) {
			cursor.buffering = true;
			read.store(discarded, std::memory_order_release);
			return false;
		}
		if (cursor.buffering || position < begin || position > end || end - position < wanted.count ||
			end - position > std::max(kPausedMaxBufferedSamples, wanted.count)) {
			cursor.buffering = true;
			const uint32_t depth = std::max(kPausedBufferSamples, wanted.count);
			// Rebuffer briefly instead of chasing samples the guest has not
			// produced. Release stale ranges so a full ring can receive new PCM.
			// Stay in this mode until full: a one-sample scheduling difference
			// must not alternate between starvation and premature recovery.
			if (end - begin < depth) {
				read.store(first, std::memory_order_release);
				return false;
			}
			position = end - depth;
		}
		result.epoch = wanted.epoch;
		result.round = wanted.round;
		result.input = wanted.input;
		result.frame = wanted.frame;
		result.reset = cursor.reset;
		result.sample_position = position;
		result.count = 0;
		r = first;
		while (r != w && reset.load(std::memory_order_acquire) == cursor.reset) {
			const auto& packet = packets[r % kPackets];
			if (packet.reset != cursor.reset || packet.count > kSamples) {
				++r;
				continue;
			}
			if (packet.epoch != wanted.epoch || packet.round != wanted.round) return false;
			const uint64_t next = result.sample_position + result.count;
			if (packet.sample_position > next) return false;
			const uint64_t offset = next - packet.sample_position;
			if (offset >= packet.count) {
				++r;
				continue;
			}
			const uint32_t count = std::min(wanted.count - result.count, packet.count - static_cast<uint32_t>(offset));
			std::memcpy(result.samples + result.count, packet.samples + offset, count * sizeof(*result.samples));
			result.serial = packet.serial;
			result.count += count;
			// A run can stop between vblanks. Leave an unconsumed suffix in
			// the queue; the next host frame continues at its next sample.
			if (offset + count == packet.count) ++r;
			if (result.count == wanted.count) {
				read.store(r, std::memory_order_release);
				return reset.load(std::memory_order_acquire) == cursor.reset;
			}
		}
		return false;
	}
};

// Briefing resumes input at different times on each viewpoint. Match actual
// requests before delivering that batch, rather than relabeling its audio.
struct GdxsvAudioInputRequest {
	std::atomic<uint32_t> sequence{0}, round{0}, seek{0};
	std::atomic<int> input{-1};

	void Publish(int requested_input, uint32_t requested_round, uint32_t requested_seek) {
		sequence.fetch_add(1, std::memory_order_acq_rel);
		// Pair with the reader's fence even when it sees a relaxed field store.
		std::atomic_thread_fence(std::memory_order_release);
		input.store(requested_input, std::memory_order_relaxed);
		round.store(requested_round, std::memory_order_relaxed);
		seek.store(requested_seek, std::memory_order_relaxed);
		sequence.fetch_add(1, std::memory_order_release);
	}
	bool Reached(int requested_input, uint32_t requested_round, uint32_t requested_seek) const {
		const uint32_t before = sequence.load(std::memory_order_acquire);
		if (before & 1) return false;
		const bool ready = input.load(std::memory_order_relaxed) >= requested_input &&
						   round.load(std::memory_order_relaxed) == requested_round &&
						   seek.load(std::memory_order_relaxed) == requested_seek;
		std::atomic_thread_fence(std::memory_order_acquire);
		return ready && before == sequence.load(std::memory_order_relaxed);
	}
};

struct GdxsvAudioShared {
	std::atomic<uint32_t> magic{0};
	GdxsvAudioInputRequest requests[kGdxsvMultiPovScreens];
	GdxsvAudioQueue guests[kGdxsvMultiPovScreens - 1];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

struct GdxsvAudioDelayed {
	uint64_t clock;
	bool paused;
	GdxsvAudioPacket packet;
};

struct GdxsvAudioStream {
	GdxsvAudioCursor cursor;
	float left = 0, right = 0, from_left = 0, from_right = 0;
	uint32_t remaining = 0;
	bool started = false, present = false, continuous = false;

	void Begin(const GdxsvAudioPacket& wanted, const GdxsvAudioPacket* packet, bool paused) {
		const bool next_present = packet != nullptr;
		const bool aligned =
			next_present && cursor.Matches(wanted, packet->reset) &&
			((paused || continuous) ? cursor.NextSample(wanted) == packet->sample_position : cursor.Next(wanted) == packet->serial);
		const bool discontinuous = present != next_present || (next_present && !aligned);
		if (started && discontinuous) {
			// Bridge from the last output sample. A missing packet has no PCM
			// to fade out, and replaying its predecessor would repeat sounds.
			from_left = left;
			from_right = right;
			remaining = kSpliceSamples;
		}
		if (next_present) cursor.Set(wanted, *packet);
		present = next_present;
		continuous = paused;
		started = true;
	}
	void Add(float sample_left, float sample_right, float& mixed_left, float& mixed_right) {
		if (remaining != 0) {
			const float weight = float(remaining--) / kSpliceSamples;
			sample_left += (from_left - sample_left) * weight;
			sample_right += (from_right - sample_right) * weight;
		}
		left = sample_left;
		right = sample_right;
		mixed_left += left;
		mixed_right += right;
	}
};

class GdxsvMultiPovAudioBackend final : public AudioBackend {
   public:
	GdxsvMultiPovAudioBackend() : AudioBackend("zz-gdxsv-multi-pov", "Internal four-screen replay mixer") {}

	void Open(AudioBackend* device) {
		if (initialized_) TermAudio();
		screen_ = gdxsv_multi_pov_screen_index();
		shared_ = static_cast<GdxsvAudioShared*>(gdxsv_multi_pov_audio_map(sizeof(GdxsvAudioShared)));
		if (shared_ == nullptr) return;
		if (screen_ == 0) {
			new (shared_) GdxsvAudioShared;
			shared_->magic.store(kAudioMagic, std::memory_order_release);
		} else if (shared_->magic.load(std::memory_order_acquire) != kAudioMagic) {
			shared_ = nullptr;
			return;
		}
		device_ = screen_ == 0 ? device : nullptr;
		if (device_ == this) device_ = nullptr;
		original_backend_ = config::AudioBackend.get();
		original_readonly_ = config::AudioBackend.isReadOnly();
		config::AudioBackend.override(slug);
		volume_ = std::clamp(config::loadInt("gdxsv", "MultiPovMixVolume", 100), 0, 100) / 100.f;
		captured_.reserve(kSamples + SAMPLE_COUNT);
		Reset();
		NOTICE_LOG(AUDIO, "multi-pov audio: %dP using one host output, three displayed frames of delay", screen_ + 1);
	}
	void Close() {
		if (shared_ == nullptr) return;
		if (screen_ != 0) shared_->guests[screen_ - 1].heartbeat.store(0, std::memory_order_release);
		shared_ = nullptr;
		captured_.clear();
		delay_.clear();
		FlushAudio();
		config::AudioBackend.reset();
		config::AudioBackend.load();
		if (original_readonly_)
			config::AudioBackend.override(original_backend_);
		else
			config::AudioBackend.set(original_backend_);
		// The existing device remains available until the emulator's normal
		// TermAudio/InitAudio lifecycle switches back to the selected backend.
	}
	bool Enabled() const { return shared_ != nullptr; }
	void Start() {
		if (!Enabled()) return;
		config::AudioBackend.override(slug);
		rate_control_ = config::loadBool("gdxsv", "AudioRateControl", true);
		settings.gdxsv.audioRateControl = false;
		settings.gdxsv.audioScale = 1.f;
		Reset();
	}

	bool init() override {
		initialized_ = true;
		InitDevice();
		ClearOutput();
		return true;  // guests intentionally have no physical output device
	}
	void term() override {
		initialized_ = false;
		if (device_ready_) device_->term();
		device_ready_ = false;
		captured_.clear();
		ClearOutput();
	}
	u32 push(const void* data, u32 frames, bool wait) override {
		if (!Enabled() || !settings.gdxsv.replayModeActive) {
			return device_ready_ ? device_->push(data, frames, wait) : 1;
		}
		++pushes_;
		const auto* samples = static_cast<const GdxsvAudioSample*>(data);
		if (captured_.size() + frames <= kSamples + SAMPLE_COUNT)
			captured_.insert(captured_.end(), samples, samples + frames);
		else
			overflow_ = true;
		return 1;
	}

	void Reset() {
		if (!Enabled()) return;
		input_pending_ = false;
		shared_->requests[screen_].Publish(-1, 0, 0);
		captured_.clear();
		delay_.clear();
		last_input_ = -1;
		have_packet_ = false;
		frame_ = 0;
		serial_ = 0;
		sample_position_ = 0;
		clock_ = 0;
		overflow_ = false;
		FlushAudio();
		ClearOutput();
		if (device_ready_) {
			device_->term();
			InitDevice();
		}
		if (screen_ != 0) shared_->guests[screen_ - 1].reset.fetch_add(1, std::memory_order_release);
	}

	bool InputPending() const { return Enabled() && initialized_ && settings.gdxsv.replayModeActive && input_pending_; }
	bool InputReady(int input, uint32_t round, uint32_t seek, bool wait) {
		if (!Enabled() || !initialized_ || !settings.gdxsv.replayModeActive) {
			input_pending_ = false;
			return true;
		}
		shared_->requests[screen_].Publish(input, round, seek);
		if (wait) {
			const auto now = std::chrono::steady_clock::now();
			if (!input_pending_) input_deadline_ = now + std::chrono::milliseconds(350);
			for (int i = 0; i < kGdxsvMultiPovScreens; ++i) {
				if (i != 0) {
					const uint32_t heartbeat = shared_->guests[i - 1].heartbeat.load(std::memory_order_acquire);
					if (heartbeat == 0 || static_cast<uint32_t>(gdxsv_audio_now_ms() - heartbeat) >= 1000) continue;
				}
				if (!shared_->requests[i].Reached(input, round, seek) && now < input_deadline_) {
					input_pending_ = true;
					return false;
				}
			}
		}
		input_pending_ = false;
		return true;
	}

	void Frame(int input, uint32_t round, uint32_t seek, int speed, bool silent, bool menu, bool paused) {
		if (!Enabled() || !initialized_ || !settings.gdxsv.replayModeActive) return;
		if (silent) {
			captured_.clear();
			FlushAudio();
		} else {
			// AudioBackend receives 512-sample blocks. Complete its final block
			// through the existing public API, then remove only our zero padding.
			// Capture runs without rate control; the combined stream is resampled.
			const uint64_t before = pushes_;
			uint32_t padding = 0;
			while (pushes_ == before && padding < SAMPLE_COUNT) {
				++padding;
				WriteSample(0, 0);
			}
			if (pushes_ == before || captured_.size() < padding)
				captured_.clear();
			else
				captured_.resize(captured_.size() - padding);
		}
		if (overflow_ || captured_.size() > kSamples) {
			WARN_LOG(AUDIO, "multi-pov audio: discarded oversized frame (%zu samples)", captured_.size());
			captured_.clear();
			overflow_ = false;
		}
		// While input is frozen, sound still advances with emulated frames.
		// Keep that count: relabeling a late guest would replay older sound.
		frame_ = input == last_input_ ? frame_ + 1 : 0;
		last_input_ = input;
		uint32_t battle = 0;
		std::string code;
		gdxsv_multi_pov_read_live_battle(code, battle);
		auto& packet = scratch_[0];
		packet.epoch = (uint64_t(battle) << 32) | seek;
		packet.serial = serial_++;
		packet.sample_position = sample_position_;
		packet.round = round;
		packet.input = input;
		packet.frame = frame_;
		if (screen_ != 0 && menu) {
			// Opening the menu can leave the screens one or two inputs apart.
			// Include that battle-frame difference in the elapsed menu time.
			GdxsvMultiPovPlayback host;
			if (gdxsv_multi_pov_read_playback(host) && host.menu_open && std::abs(input - host.seek_target) <= 2 &&
				static_cast<int64_t>(packet.frame) + input - host.seek_target >= 0) {
				packet.frame = static_cast<uint32_t>(static_cast<int64_t>(packet.frame) + input - host.seek_target);
				packet.input = static_cast<int>(host.seek_target);
			}
		}
		packet.count = static_cast<uint32_t>(captured_.size());
		sample_position_ += packet.count;
		std::memcpy(packet.samples, captured_.data(), packet.count * sizeof(*packet.samples));
		captured_.clear();
		if (screen_ != 0) {
			if (have_packet_ && packet.Compare(previous_packet_) <= 0)
				shared_->guests[screen_ - 1].reset.fetch_add(1, std::memory_order_release);
			previous_packet_.epoch = packet.epoch;
			previous_packet_.round = packet.round;
			previous_packet_.input = packet.input;
			previous_packet_.frame = packet.frame;
			have_packet_ = true;
			shared_->guests[screen_ - 1].Push(packet);
			return;
		}
		if (!delay_.empty() && (packet.epoch != delay_.back().packet.epoch || packet.Compare(delay_.back().packet) <= 0)) delay_.clear();
		// Six units represent 1x, 2x and 3x exactly, including speed changes.
		clock_ += 6 / (1 + std::clamp(speed, 0, 2));
		delay_.emplace_back();
		delay_.back().clock = clock_;
		delay_.back().paused = paused;
		delay_.back().packet.Copy(packet);
		while (!delay_.empty() && clock_ - delay_.front().clock >= 18) {
			packet.Copy(delay_.front().packet);
			const bool delayed_pause = delay_.front().paused;
			delay_.pop_front();
			Mix(packet, delayed_pause);
			if (packet.count != 0) break;
		}
	}

   private:
	void InitDevice() {
		device_ready_ = device_ != nullptr && device_->init();
		if (!device_ready_ && device_ != nullptr && original_backend_ != "auto") {
			WARN_LOG(AUDIO, "multi-pov audio: driver %s failed to initialize; trying auto", device_->slug.c_str());
			device_ = AudioBackend::getBackend("auto");
			if (device_ == this) device_ = nullptr;
			device_ready_ = device_ != nullptr && device_->init();
		}
		if (screen_ == 0 && !device_ready_) WARN_LOG(AUDIO, "multi-pov audio: running without host audio output");
	}

	void ClearOutput() {
		pending_.clear();
		streams_ = {};
		paused_audio_ = false;
		phase_ = 0;
		step_ = 1;
		queue_level_ = .5;
		previous_ = {};
		primed_ = false;
		have_previous_ = false;
	}
	void Mix(const GdxsvAudioPacket& wanted, bool paused) {
		// Resume can spend another run waiting on the paused input. Its local
		// frame counters still differ; realign only after that input advances.
		if (paused) {
			paused_audio_ = true;
			paused_epoch_ = wanted.epoch;
			paused_round_ = wanted.round;
			paused_input_ = wanted.input;
		} else if (wanted.epoch != paused_epoch_ || wanted.round != paused_round_ || wanted.input != paused_input_) {
			paused_audio_ = false;
		}
		if (paused_audio_ && wanted.count == 0) return;
		uint32_t mask = 1;
		uint32_t expected_mask = 1;
		int active = 1;
		for (int i = 1; i < kGdxsvMultiPovScreens; ++i) {
			auto& queue = shared_->guests[i - 1];
			const uint32_t heartbeat = queue.heartbeat.load(std::memory_order_acquire);
			if (heartbeat != 0 && static_cast<uint32_t>(gdxsv_audio_now_ms() - heartbeat) < 1000) {
				++active;
				expected_mask |= 1u << i;
			}
			if (queue.Take(wanted, scratch_[i], &streams_[i - 1].cursor, paused_audio_) && (wanted.count == 0 || scratch_[i].count != 0)) {
				mask |= 1u << i;
				if (wanted.count == 0) streams_[i - 1].cursor.Set(wanted, scratch_[i]);
			}
		}
		if (wanted.count == 0 || !device_ready_) return;
		for (int i = 1; i < kGdxsvMultiPovScreens; ++i)
			streams_[i - 1].Begin(wanted, mask & (1u << i) ? &scratch_[i] : nullptr, paused_audio_);
		if ((mask & expected_mask) != expected_mask) ++incomplete_;
		if (++logged_frames_ == 300) {
			NOTICE_LOG(AUDIO, "multi-pov audio: %u/300 frames with missing PCM", incomplete_);
			logged_frames_ = incomplete_ = 0;
		}
		const float target = volume_ / active;
		if (gain_ < 0) gain_ = target;
		uint32_t queued = 0, capacity = 0;
		const bool measured = device_->getQueueLevel(queued, capacity) && capacity != 0;
		if (measured) {
			queue_level_ += .05 * (std::min(1., double(queued) / capacity) - queue_level_);
			step_ = rate_control_ ? 1. / (1. + .005 * (1. - 2. * queue_level_)) : 1.;
			if (queued == 0) primed_ = false;
		}
		for (uint32_t j = 0; j < wanted.count; ++j) {
			float left = 0, right = 0;
			for (int i = 0; i < kGdxsvMultiPovScreens; ++i) {
				const auto& packet = scratch_[i];
				float sample_left = 0, sample_right = 0;
				if (mask & (1u << i)) {
					const float position = wanted.count > 1 ? float(j) * (packet.count - 1) / (wanted.count - 1) : 0;
					const uint32_t a = std::min(uint32_t(position), packet.count - 1);
					const uint32_t b = std::min(a + 1, packet.count - 1);
					const float t = position - a;
					sample_left = packet.samples[a].left + (packet.samples[b].left - packet.samples[a].left) * t;
					sample_right = packet.samples[a].right + (packet.samples[b].right - packet.samples[a].right) * t;
				}
				if (i == 0) {
					left += sample_left;
					right += sample_right;
				} else {
					streams_[i - 1].Add(sample_left, sample_right, left, right);
				}
			}
			// Keep gain stable for transient missing frames; smooth lost/rejoining
			// participants over about 75ms, without clipping during the change.
			gain_ += std::clamp(target - gain_, -1.f / 3308, 1.f / 3308);
			const GdxsvAudioSample sample{static_cast<int16_t>(std::clamp(left * gain_, -32768.f, 32767.f)),
										  static_cast<int16_t>(std::clamp(right * gain_, -32768.f, 32767.f))};
			if (!have_previous_) {
				previous_ = sample;
				have_previous_ = true;
			}
			while (phase_ < 1) {
				pending_.push_back({static_cast<int16_t>(previous_.left + (sample.left - previous_.left) * phase_),
									static_cast<int16_t>(previous_.right + (sample.right - previous_.right) * phase_)});
				phase_ += step_;
			}
			phase_ -= 1;
			previous_ = sample;
		}
		const size_t prebuffer = measured ? std::clamp<size_t>(capacity / 2, SAMPLE_COUNT, SAMPLE_COUNT * 4) : SAMPLE_COUNT * 2;
		if (!primed_ && pending_.size() < prebuffer) return;
		primed_ = true;
		size_t sent = 0;
		while (pending_.size() - sent >= SAMPLE_COUNT) {
			device_->push(pending_.data() + sent, SAMPLE_COUNT, false);
			sent += SAMPLE_COUNT;
		}
		pending_.erase(pending_.begin(), pending_.begin() + sent);
	}

	GdxsvAudioShared* shared_ = nullptr;
	AudioBackend* device_ = nullptr;
	std::string original_backend_;
	bool original_readonly_ = false;
	int screen_ = -1;
	bool initialized_ = false, device_ready_ = false, overflow_ = false;
	uint64_t pushes_ = 0, clock_ = 0, serial_ = 0, sample_position_ = 0;
	int last_input_ = -1;
	uint32_t frame_ = 0, logged_frames_ = 0, incomplete_ = 0;
	std::vector<GdxsvAudioSample> captured_, pending_;
	std::array<GdxsvAudioPacket, kGdxsvMultiPovScreens> scratch_;
	std::array<GdxsvAudioStream, kGdxsvMultiPovScreens - 1> streams_;
	GdxsvAudioPacket previous_packet_;
	bool have_packet_ = false;
	bool rate_control_ = true;
	bool paused_audio_ = false;
	uint64_t paused_epoch_ = 0;
	uint32_t paused_round_ = 0;
	int paused_input_ = -1;
	bool input_pending_ = false;
	std::chrono::steady_clock::time_point input_deadline_{};
	std::deque<GdxsvAudioDelayed> delay_;
	float volume_ = 1, gain_ = -1;
	double phase_ = 0, step_ = 1, queue_level_ = .5;
	GdxsvAudioSample previous_{};
	bool primed_ = false, have_previous_ = false;
};

struct GdxsvAudioState {
	std::unique_ptr<GdxsvMultiPovAudioBackend> backend;
};
GdxsvAudioState gdxsv_audio;
}  // namespace

bool gdxsv_multi_pov_audio_open() {
	if (!config::loadBool("gdxsv", "MultiPovAudioSync", true) || gdxsv_headless()) return true;
	AudioBackend* device = AudioBackend::getBackend(config::AudioBackend.get());
	if (device == nullptr) device = AudioBackend::getBackend("auto");
	if (!gdxsv_audio.backend) gdxsv_audio.backend = std::make_unique<GdxsvMultiPovAudioBackend>();
	gdxsv_audio.backend->Open(device);
	return gdxsv_audio.backend->Enabled();
}

void gdxsv_multi_pov_audio_close() {
	if (gdxsv_audio.backend) gdxsv_audio.backend->Close();
}

bool gdxsv_multi_pov_audio_enabled() { return gdxsv_audio.backend && gdxsv_audio.backend->Enabled(); }

void gdxsv_multi_pov_audio_start() {
	if (gdxsv_audio.backend) gdxsv_audio.backend->Start();
}

void gdxsv_multi_pov_audio_frame(int input, uint32_t round, uint32_t seek, int speed, bool silent, bool menu, bool paused) {
	if (gdxsv_audio.backend) gdxsv_audio.backend->Frame(input, round, seek, speed, silent, menu, paused);
}

void gdxsv_multi_pov_audio_reset() {
	if (gdxsv_audio.backend) gdxsv_audio.backend->Reset();
}

bool gdxsv_multi_pov_audio_input_pending() { return gdxsv_audio.backend && gdxsv_audio.backend->InputPending(); }

bool gdxsv_multi_pov_audio_input_ready(int input, uint32_t round, uint32_t seek, bool wait) {
	return !gdxsv_audio.backend || gdxsv_audio.backend->InputReady(input, round, seek, wait);
}

#else
bool gdxsv_multi_pov_audio_open() { return true; }
void gdxsv_multi_pov_audio_close() {}
bool gdxsv_multi_pov_audio_enabled() { return false; }
void gdxsv_multi_pov_audio_start() {}
void gdxsv_multi_pov_audio_frame(int, uint32_t, uint32_t, int, bool, bool, bool) {}
void gdxsv_multi_pov_audio_reset() {}
bool gdxsv_multi_pov_audio_input_pending() { return false; }
bool gdxsv_multi_pov_audio_input_ready(int, uint32_t, uint32_t, bool) { return true; }
#endif
