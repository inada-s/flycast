#include "gdxsv_spectator_downlink.h"

#include <chrono>
#include <limits>
#include <thread>

#include "gdxsv_network.h"
#include "sleep.h"
#include "types.h"

namespace {
// How often SpectatorSubscribeRequest is resent as a keepalive/resume-point
// refresh - must stay comfortably under LBS's spectatorSubscriberTimeout
// (10s, see lbs_spectator.go) so a brief delay never drops the subscription.
constexpr int kSubscribeIntervalMs = 2000;
constexpr size_t kSubscribeCookieBytes = 16;
// Protocol limit shared with maxSpectatorPushFrames in lbs_spectator.go;
// changes require client/server coordination, including the queue budget below.
// LBS sends the entire remaining input tail when it is smaller than this limit.
constexpr int kInputPushFrames = 128;
// Pause read-ahead when emulation cannot drain it. At full size this queue
// holds 256 KiB of input values, plus metadata and protobuf object overhead.
constexpr size_t kMaxPendingPushes = 256;
}  // namespace

void GdxsvSpectatorDownlink::Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code,
									int32_t from_frame) {
	if (running_ || lbs_host.empty() || lbs_port == 0 || battle_code.empty()) {
		return;
	}
	Stop(); // Join an old worker, including one that failed during startup.
	applied_round_state_version_ = 0;

	{
		std::lock_guard<std::mutex> lock(mtx_);
		pending_.clear();
		have_header_ = false;
		patches_.clear();
		patch_total_ = -1;
		acked_frame_ = from_frame;
		initial_download_ = true;
		acked_round_state_version_ = 0;
		acked_dirty_ = false;
	}
	running_ = true;
	thread_ = std::thread([this, lbs_host, lbs_port, battle_code, from_frame]() {
		ThreadMain(lbs_host, lbs_port, battle_code, from_frame);
	});
}

void GdxsvSpectatorDownlink::Stop() {
	running_ = false;
	if (thread_.joinable()) thread_.join();
}

bool GdxsvSpectatorDownlink::WaitForBootstrap(proto::BattleLogFile *out, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (running_ && std::chrono::steady_clock::now() < deadline) {
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (have_header_ && patch_total_ >= 0 && static_cast<int32_t>(patches_.size()) == patch_total_) {
				*out = header_;
				for (const auto &patch : patches_) {
					*out->add_patches() = patch;
				}
				return true;
			}
		}
		sleep_us(2000);
	}
	return false;
}

bool GdxsvSpectatorDownlink::DrainInto(proto::BattleLogFile *log_file, bool *backlog_pending) {
	std::deque<proto::SpectatorInputPush> pushes;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		pushes.swap(pending_);
	}

	bool applied = false;
	while (!pushes.empty()) {
		const auto &push = pushes.front();
		const int32_t have = log_file->inputs_size();

		if (0 < push.inputs_size()) {
			if (push.start_frame() > have) {
				// The worker only stages contiguous inputs. The caller must use
				// the same log throughout a session; retain this batch if that
				// contract is broken, since queued inputs may already be ACKed.
				WARN_LOG(COMMON, "spectator downlink gap: start_frame=%d have=%d", push.start_frame(), have);
				std::lock_guard<std::mutex> lock(mtx_);
				for (auto &pending : pending_) pushes.push_back(std::move(pending));
				pending_.swap(pushes);
				return applied;
			}
			const int32_t offset = have - push.start_frame();
			for (int32_t i = offset; i < push.inputs_size(); ++i) {
				log_file->add_inputs(push.inputs(i));
			}
			if (offset < push.inputs_size()) {
				applied = true;
				if (backlog_pending)
					*backlog_pending = push.inputs_size() >= kInputPushFrames;
			}
		}

		// Results change without another round start. A version also keeps a
		// delayed older snapshot from overwriting a result already applied.
		// Close-only pushes carry the required version, but no snapshot.
		if (push.close_reason().empty() && push.round_state_version() > applied_round_state_version_) {
			log_file->mutable_start_msg_indexes()->CopyFrom(push.start_msg_indexes());
			log_file->mutable_start_msg_randoms()->CopyFrom(push.start_msg_randoms());
			log_file->mutable_round_data()->CopyFrom(push.round_data());
			applied_round_state_version_ = push.round_state_version();
			{
				std::lock_guard<std::mutex> lock(mtx_);
				acked_round_state_version_ = applied_round_state_version_;
				acked_dirty_ = true;
			}
			applied = true;
		}
		if (!push.close_reason().empty() && log_file->close_reason().empty() &&
			log_file->inputs_size() >= push.start_frame() &&
			applied_round_state_version_ >= push.round_state_version()) {
			log_file->set_close_reason(push.close_reason());
			log_file->set_disconnect_user_index(push.disconnect_user_index());
			applied = true;
		}
		pushes.pop_front();
	}

	return applied;
}

void GdxsvSpectatorDownlink::ReportAcked(int32_t frame, bool initial_download) {
	std::lock_guard<std::mutex> lock(mtx_);
	initial_download_ = initial_download_ && initial_download;
	if (frame > acked_frame_) {
		acked_frame_ = frame;
		acked_dirty_ = true;
	}
}

void GdxsvSpectatorDownlink::ThreadMain(std::string lbs_host, int lbs_port, std::string battle_code,
										 int32_t from_frame) {
	NOTICE_LOG(COMMON, "Start GdxsvSpectatorDownlink Thread battle_code=%s", battle_code.c_str());

	UdpClient client;
	if (!client.Bind(0)) {
		WARN_LOG(COMMON, "GdxsvSpectatorDownlink client.Bind failed");
		running_ = false;
		return;
	}

	UdpRemote remote;
	if (!remote.Open(lbs_host.c_str(), lbs_port)) {
		WARN_LOG(COMMON, "GdxsvSpectatorDownlink remote.Open failed");
		running_ = false;
		return;
	}

	// The first request reserves enough bytes for the stateless challenge.
	// Keep the echoed cookie local to this battle's socket/thread.
	std::string cookie(kSubscribeCookieBytes, '\0');
	auto send_subscribe = [&](int32_t from) {
		proto::Packet pkt;
		pkt.set_type(proto::MessageType::SpectatorSubscribeType);
		auto *sub = pkt.mutable_spectator_subscribe_data();
		sub->set_battle_code(battle_code);
		sub->set_from_frame(from);
		sub->set_cookie(cookie);

		char buf[256];
		if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
			client.SendTo(buf, pkt.GetCachedSize(), remote);
		}
	};
	auto send_ack = [&]() {
		proto::Packet pkt;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (!acked_dirty_) return;
			pkt.set_type(proto::MessageType::SpectatorInputAckType);
			auto *ack = pkt.mutable_spectator_input_ack_data();
			ack->set_battle_code(battle_code);
			ack->set_ack_frame(acked_frame_);
			ack->set_patch_ack(static_cast<int32_t>(patches_.size()));
			ack->set_round_ack(acked_round_state_version_);
			acked_dirty_ = false;
		}
		char buf[256];
		if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
			client.SendTo(buf, pkt.GetCachedSize(), remote);
		}
	};

	int32_t received_frame = from_frame;
	int32_t received_round_state_version = 0;
	bool oversized_push_warned = false;
	auto stage_push = [&](proto::SpectatorInputPush &push) {
		if (push.inputs_size() > kInputPushFrames && !oversized_push_warned) {
			WARN_LOG(COMMON, "spectator downlink rejected oversized inputs: battle_code=%s start_frame=%d inputs=%d limit=%d (once per session)",
				battle_code.c_str(), push.start_frame(), push.inputs_size(), kInputPushFrames);
			oversized_push_warned = true;
		}
		std::lock_guard<std::mutex> lock(mtx_);
		// Duplicate/lost ACKs need feedback too, even when no new data fits.
		acked_dirty_ = true;
		if (push.has_header()) {
			header_ = push.header();
			// Completion belongs to the final, ACK-gated push, never the
			// bootstrap metadata of an already-finished battle.
			header_.clear_close_reason();
			header_.clear_disconnect_user_index();
			have_header_ = true;
			patch_total_ = push.patch_total(); // Includes the zero-patch case.
		}
		if (0 < push.patches_size() || 0 < push.patch_total()) {
			patch_total_ = push.patch_total();
			if (push.patch_start() == static_cast<int32_t>(patches_.size())) {
				for (int i = 0; i < push.patches_size(); ++i) patches_.push_back(push.patches(i));
			}
		}

		const int64_t end = static_cast<int64_t>(push.start_frame()) + push.inputs_size();
		const bool new_inputs = push.start_frame() >= 0 && push.start_frame() <= received_frame &&
			push.inputs_size() <= kInputPushFrames && end <= std::numeric_limits<int32_t>::max() && end > received_frame;
		const bool new_round = push.close_reason().empty() && push.round_state_version() > received_round_state_version;
		if (pending_.size() < kMaxPendingPushes && (new_inputs || new_round || !push.close_reason().empty())) {
			// A gap, duplicate or invalid range must not advance the ACK. Round
			// metadata can still be retained independently of missing inputs.
			if (!new_inputs) push.clear_inputs();
			const int32_t version = push.round_state_version();
			pending_.push_back(std::move(push));
			if (new_inputs) received_frame = static_cast<int32_t>(end);
			if (new_round) received_round_state_version = version;
		}
		// Keep ACK=0 until bootstrap succeeds, so a lost header still gets
		// requested by the existing from_frame=0 keepalive recovery path.
		if (initial_download_ && have_header_ && patch_total_ >= 0 &&
			static_cast<int32_t>(patches_.size()) == patch_total_ && received_frame > acked_frame_) {
			acked_frame_ = received_frame;
		}
		return initial_download_;
	};

	send_subscribe(from_frame);
	int64_t last_subscribe_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count();

	// LBS never splits a patch, so a patch chunk can be several KB.
	std::vector<char> buf(65536);
	while (running_) {
		for (int received = 0; running_ && received < 64; ++received) {
			sockaddr_storage sender{};
			socklen_t addrlen = sizeof(sender);
			int n = client.RecvFrom(buf.data(), static_cast<int>(buf.size()), &sender, &addrlen);
			if (n <= 0) break;
			if (!is_same_addr(reinterpret_cast<const sockaddr *>(&sender), remote.net_addr())) continue;

			proto::Packet pkt;
			if (!pkt.ParseFromArray(buf.data(), n)) continue;
			if (pkt.type() == proto::MessageType::SpectatorSubscribeChallengeType) {
				const auto &challenge = pkt.spectator_subscribe_challenge_data();
				if (challenge.battle_code() != battle_code || challenge.cookie().size() != kSubscribeCookieBytes) continue;
				cookie = challenge.cookie();
				int32_t from;
				{
					std::lock_guard<std::mutex> lock(mtx_);
					from = acked_frame_;
				}
				send_subscribe(from);
				last_subscribe_ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
						.count();
				continue;
			}
			if (pkt.type() != proto::MessageType::SpectatorInputPushType) continue;
			if (pkt.spectator_input_push_data().battle_code() != battle_code) continue;

			// During startup the next chunk need not wait for an emulated
			// frame or the rest of the receive batch to finish first.
			if (stage_push(*pkt.mutable_spectator_input_push_data())) send_ack();
		}

		send_ack(); // Application may have advanced while no new packet arrived.

		const int64_t now_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count();
		if (kSubscribeIntervalMs <= now_ms - last_subscribe_ms) {
			int32_t from;
			{
				std::lock_guard<std::mutex> lock(mtx_);
				from = acked_frame_;
			}
			send_subscribe(from);
			last_subscribe_ms = now_ms;
		}

		sleep_us(1000);
	}

	client.Close();
	NOTICE_LOG(COMMON, "End GdxsvSpectatorDownlink Thread");
}
