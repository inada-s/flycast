#include "gdxsv.h"

#include <xxhash.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <random>
#include <sstream>

#include "cfg/option.h"
#include "emulator.h"
#include "gdx_rpc.h"
#include "gdxsv_key_display.h"
#include "gdxsv_patch_abi.h"
#include "gdxsv_prof.h"
#include "gdxsv_translation.h"
#include "hw/sh4/sh4_if.h"
#include "imgui/imgui.h"
#include "libs.h"
#include "log/InMemoryListener.h"
#include "log/LogManager.h"
#include "network/ggpo.h"
#include "oslib/oslib.h"
#include "reios/reios.h"
#include "oslib/http_client.h"
#include "ui/gui.h"
#include "version.h"

bool encode_zlib_deflate(const char *data, int len, std::vector<u8> &out) {
	z_stream z{};
	int ret = deflateInit(&z, Z_DEFAULT_COMPRESSION);
	if (ret == Z_OK) {
		bool ok = false;
		char zbuf[1024];
		z.next_in = (Bytef *)data;
		z.avail_in = (uInt)len;
		for (;;) {
			z.next_out = (Bytef *)zbuf;
			z.avail_out = sizeof(zbuf);
			ret = deflate(&z, 0 < z.avail_in ? Z_NO_FLUSH : Z_FINISH);
			if (ret != Z_OK && ret != Z_STREAM_END) {
				ERROR_LOG(COMMON, "zlib serialize error: %d", ret);
				break;
			}
			std::copy_n(zbuf, sizeof(zbuf) - z.avail_out, std::back_inserter(out));
			if (ret == Z_STREAM_END) {
				ok = true;
				break;
			}
		}
		deflateEnd(&z);
		return ok;
	}
	return false;
}

// DC2 functions that render_current_frame (0c052244) calls only to produce output. On skip frames
// (rollback resimulation, replay seek) the render call itself still runs, because the object draw callbacks
// inside it update state that game logic reads (part/bone world positions); only these return at entry.
// Picked by profiling a replay seek, and each checked to leave game RAM equal to a full render.
static const u32 dc2RenderOnlyFuncs[] = {
	0x0c1ab040, 0x0c1ab042,                                      // vertex transform + polygon output
	0x0c19f910, 0x0c19fa60, 0x0c19fa90, 0x0c19fac0, 0x0c19fb00,  // model draw
	0x0c16d504,                                                  // stage draw
	0x0c076bb0,                                                  // MS (PlayerWork +0x78) draw callback
	0x0c197f80, 0x0c198030, 0x0c19ac00, 0x0c19d2d0,              // per-pass view setup
	0x0c16c880, 0x0c16c8d0,                                      // 2D pass
};

// The same functions in DC1 (render_current_frame 0c064e24), matched to DC2 by code and call structure.
static const u32 dc1RenderOnlyFuncs[] = {
	0x0c14b860, 0x0c14b862,                                      // vertex transform + polygon output
	0x0c140130, 0x0c140280, 0x0c1402b0, 0x0c1402e0, 0x0c140320,  // model draw
	0x0c115abc,                                                  // stage draw
	0x0c08092e,                                                  // MS (PlayerWork +0x78) draw callback
	0x0c1387a0, 0x0c138850, 0x0c13b420, 0x0c13daf0,              // per-pass view setup
	0x0c115074, 0x0c1150c4,                                      // 2D pass
};

bool Gdxsv::InGame() const { return enabled_ && (netmode_ == NetMode::McsUdp || netmode_ == NetMode::McsRollback); }

bool Gdxsv::IsOnline() const {
	return enabled_ &&
		   (netmode_ == NetMode::McsUdp || netmode_ == NetMode::McsRollback || netmode_ == NetMode::Lbs || lbs_net_.IsConnected());
}

bool Gdxsv::IsSaveStateAllowed() const { return netmode_ == NetMode::Offline; }

bool Gdxsv::IsReplaying() const { return netmode_ == NetMode::Replay; }

bool Gdxsv::IsReplaySeeking() const { return IsReplaying() && replay_net_.Seeking(); }

bool Gdxsv::Enabled() const { return enabled_; }

void Gdxsv::DisplayOSD() {
	rollback_net_.DisplayOSD();
	replay_net_.DisplayOSD();
	key_display_.DisplayOSD();
	projectile_view_.DisplayOSD();
	slowdown_.DisplayOSD();
}

const char *Gdxsv::NetModeString() const {
	if (netmode_ == NetMode::Offline) return "Offline";
	if (netmode_ == NetMode::Lbs) return "Lbs";
	if (netmode_ == NetMode::McsUdp) return "McsUdp";
	if (netmode_ == NetMode::McsRollback) return "McsRollback";
	if (netmode_ == NetMode::Replay) return "Replay";
	return "Unknown";
}

void Gdxsv::Reset() {
	InvalidateWidescreenPatch();
	lbs_net_.Reset();
	udp_net_.Reset();
	rollback_net_.Reset();
	replay_net_.Reset();
	netmode_ = NetMode::Offline;
	http::init();
	settings.gdxsv.disk = 0;
	settings.gdxsv.replayModeActive = false;
	settings.gdxsv.skipRenderingBaseAddr = 0;
	settings.gdxsv.renderOnlyFuncs = nullptr;
	settings.gdxsv.renderOnlyFuncCount = 0;

	// Automatically add ContentPath if it is empty.
	if (config::ContentPath.get().empty()) {
		config::ContentPath.get().push_back("./");
	}

	auto game_id = std::string(ip_meta.product_number, sizeof(ip_meta.product_number));
	if (game_id != "T13306M   ") {
		enabled_ = false;
		return;
	}
	enabled_ = true;
	std::string disk_num(ip_meta.disk_num, 1);
	if (disk_num == "1") disk_ = 1;
	if (disk_num == "2") disk_ = 2;
	settings.gdxsv.disk = disk_;
	settings.gdxsv.skipRenderingBaseAddr = (disk_ == 1) ? 0x0c064cce : (disk_ == 2) ? 0x0c0520e2 : 0;
	if (disk_ == 1) {
		settings.gdxsv.renderOnlyFuncs = dc1RenderOnlyFuncs;
		settings.gdxsv.renderOnlyFuncCount = std::size(dc1RenderOnlyFuncs);
	}
	else if (disk_ == 2) {
		settings.gdxsv.renderOnlyFuncs = dc2RenderOnlyFuncs;
		settings.gdxsv.renderOnlyFuncCount = std::size(dc2RenderOnlyFuncs);
	}

	RestoreOnlinePatch();

	maxrebattle_ = 5;

#ifdef __APPLE__
	signal(SIGPIPE, SIG_IGN);
#endif

	if (config::GdxLocalPort == 0) {
		config::GdxLocalPort = get_random_port_number();
	}

	if (config::GdxLoginKey.get().empty()) {
		config::GdxLoginKey = GenerateLoginKey();
		config::GdxLoginKey.save();
	}

	NOTICE_LOG(COMMON, "gdxsv disk:%d server:%s loginkey:%s udp_port:%d", (int)disk_, config::GdxLobbyServer.get().c_str(),
			   config::GdxLoginKey.get().c_str(), config::GdxLocalPort.get());

	lbs_net_.lbs_packet_filter([this](const LbsMessage &lbs_msg) -> bool {
		if (netmode_ != NetMode::Lbs) {
			if (lbs_net_.IsConnected()) {
				// The user is not in lobby but connection is alive
				if (lbs_msg.command == LbsMessage::lbsLineCheck) {
					std::vector<u8> buf;
					LbsMessage::ClAnswer(lbs_msg).Serialize(buf);
					lbs_net_.Send(buf);
				}
			}

			return false;
		}

		if (lbs_msg.command == LbsMessage::lbsUserRegist || lbs_msg.command == LbsMessage::lbsUserDecide) {
			std::string id(6, ' ');
			for (int i = 0; i < 6; i++) {
				id[i] = lbs_msg.body[2 + i];
			}
			user_id_ = id;
			lbs_net_.Send(GenerateP2PMatchReportPacket());
			NotifyWanPort();
		}

		if (lbs_msg.command == LbsMessage::lbsLobbyMatchingEntry) {
			NotifyWanPort();
		}

		if (lbs_msg.command == LbsMessage::lbsUserRegist || lbs_msg.command == LbsMessage::lbsUserDecide ||
			lbs_msg.command == LbsMessage::lbsLineCheck) {
			lbs_net_.Send(GeneratePlatformInfoPacket());
		}

		if (lbs_msg.command == LbsMessage::lbsLineCheck) {
			if (gui_is_open() || gui_state == GuiState::VJoyEdit) {
				return false;
			}
		}

		if (lbs_msg.command == LbsMessage::lbsReadyBattle) {
			// Reset current patches for no-patched game
			RestoreOnlinePatch();
			going_to_battle_ = true;
		}

		if (lbs_msg.command == LbsMessage::lbsP2PMatching) {
			proto::P2PMatching matching;
			if (matching.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
				rollback_net_.Prepare(matching, config::GdxLocalPort);
			} else {
				ERROR_LOG(COMMON, "p2p matching deserialize error");
			}

			return false;
		}

		if (lbs_msg.command == LbsMessage::lbsGamePatch) {
			// Reset current patches and update patch_list
			RestoreOnlinePatch();
			if (patch_list_.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
				ApplyOnlinePatch(true);
			} else {
				ERROR_LOG(COMMON, "patch_list deserialize error");
			}

			return false;
		}

		if (lbs_msg.command == LbsMessage::lbsBattleUserCount && disk_ == 2 && GdxsvLanguage::Language() != GdxsvLanguage::Lang::Disabled) {
			u32 battle_user_count = u32(lbs_msg.body[0]) << 24 | u32(lbs_msg.body[1]) << 16 | u32(lbs_msg.body[2]) << 8 | lbs_msg.body[3];
			const u32 offset = 0x8C000000 + 0x00010000;
			gdxsv_WriteMem32(offset + 0x3839FC, battle_user_count);

			return false;
		}

		return true;
	});
}

bool Gdxsv::HookOpenMenu() {
	if (!enabled_) return true;

	if (InGame()) {
		if (netmode_ == NetMode::McsRollback) {
			rollback_net_.ToggleNetworkStat();
		}
		return false;
	}

	if (netmode_ == NetMode::Replay) {
		return replay_net_.OnOpenMenu();
	}

	return true;
}

void Gdxsv::HookVBlank() {
	if (netmode_ != NetMode::Lbs && lbs_net_.IsConnected()) {
		lbs_net_.OnSockPoll();
	}
	if (!ggpo::active()) {
		// Don't edit memory at vsync if ggpo::active
		WritePatch();
	}
	if (disk_ == 2) {
		slowdown_.OnVBlank();
	}
}

void Gdxsv::HookEndOfFrame() {
	if (netmode_ == NetMode::Replay) {
		gdxsv.replay_net_.OnEndOfFrame();
	}
}

void Gdxsv::HookNextFrame() {
	// Apply viewport changes on the emulation thread between SH4 runs,
	// keeping guest-memory writes and code-cache resets off the SDL thread.
	WriteWidescreenPatch();
	if (netmode_ == NetMode::McsRollback) {
		gdxsv.rollback_net_.OnNextFrame();
	}
	if (netmode_ == NetMode::Replay) {
		gdxsv.replay_net_.OnNextFrame();
	}
}

void Gdxsv::HookMainUiLoop() {
	if (!enabled_) return;

	// Stop fast-forward from lbsReadyBattle on: the P2P ping test that follows would measure an inflated RTT.
	if (InGame() || going_to_battle_) {
		settings.input.fastForwardMode = false;
	}

	if (netmode_ == NetMode::McsRollback) {
		gdxsv.rollback_net_.OnMainUiLoop();
	}

	if (netmode_ == NetMode::Replay) {
		gdxsv.replay_net_.OnMainUiLoop();
	}

	if (gui_is_open() || gui_state == GuiState::VJoyEdit) {
		if (netmode_ == NetMode::Lbs && lbs_net_.IsConnected()) {
			// Prevent disconnection from lobby server
			lbs_net_.OnSockPoll();
		}
	}
}

std::vector<u8> Gdxsv::GeneratePlatformInfoPacket() {
	std::stringstream ss;
	// Without this, numeric fields below (e.g. udp_port) pick up the
	// process-global locale set by i18n.cpp's std::locale::global(""),
	// which under some locales inserts thousands separators (e.g.
	// "58,348") that the server can't parse back as an integer.
	ss.imbue(std::locale::classic());
	ss << "cpu="
	   <<
#if HOST_CPU == CPU_X86
		"x86"
#elif HOST_CPU == CPU_ARM
		"ARM"
#elif HOST_CPU == CPU_MIPS
		"MIPS"
#elif HOST_CPU == CPU_X64
		"x86/64"
#elif HOST_CPU == CPU_GENERIC
		"Generic"
#elif HOST_CPU == CPU_ARM64
		"ARM64"
#else
		"Unknown"
#endif
	   << "\n";
	ss << "os="
	   <<
#ifdef __ANDROID__
		"Android"
#elif defined(__unix__)
		"Linux"
#elif defined(__APPLE__)
#ifdef TARGET_IPHONE
		"iOS"
#else
		"macOS"
#endif
#elif defined(_WIN32)
		"Windows"
#else
		"Unknown"
#endif
	   << "\n";
	ss << "flycast=" << GIT_VERSION << "\n";
	ss << "git_hash=" << GIT_HASH << "\n";
	ss << "build_date=" << BUILD_DATE << "\n";
	ss << "disk=" << (int)disk_ << "\n";
	ss << "wireless=" << (int)(os_GetConnectionMedium() == "Wireless") << "\n";
	ss << "patch_id=" << symbols_[":patch_id"] << "\n";
	ss << "language=" << GdxsvLanguage::TextureDirectoryName() << "\n";
	ss << "local_ip=" << lbs_net_.LocalIP() << "\n";
	ss << "udp_port=" << config::GdxLocalPort << "\n";
	std::string machine_id = os_GetMachineID();
	if (machine_id.length()) {
		auto digest = XXH64(machine_id.c_str(), machine_id.size(), 37);
		ss << "machine_id=" << std::hex << digest << std::dec << "\n";
	}

	if (gcp_ping_test_result_.valid()) {
		for (const auto &res : gcp_ping_test_result_.get()) {
			ss << res.first << "=" << res.second << "\n";
		}
	}

	if (public_ipv4_.valid()) {
		if (public_ipv4_.get().first) {
			ss << "public_ipv4=" << public_ipv4_.get().second << "\n";
		}
	}

	if (public_ipv6_.valid()) {
		if (public_ipv6_.get().first) {
			ss << "public_ipv6=" << public_ipv6_.get().second << "\n";
		}
	}

	if (future_is_ready(p2p_feasibility_result_)) {
		const auto& res = p2p_feasibility_result_.get();
		ss << "port_test_v4=" << res.port_test_v4 << "\n";
		
		if (!res.upnp_result.empty() && upnp_.isInitialized()) {
			ss << "upnp_result=" << res.upnp_result << "\n";
			ss << "upnp_local_ip=" << upnp_.localAddress() << "\n";
			ss << "upnp_public_ip=" << upnp_.externalAddress() << "\n";
			ss << "upnp_local_ip_differ=" << static_cast<int>(lbs_net_.LocalIP() != std::string(upnp_.localAddress())) << "\n";
			if (public_ipv4_.valid()) {
				if (public_ipv4_.get().first) {
					ss << "upnp_public_ip_differ=" << static_cast<int>(public_ipv4_.get().second != std::string(upnp_.externalAddress()))
					<< "\n";
				}
			}
		}
	}

	const auto raw_content = ss.str();
	std::vector<u8> content;
	if (!encode_zlib_deflate(raw_content.c_str(), raw_content.size(), content)) {
		content.assign(raw_content.begin(), raw_content.end());
	}

	std::vector<u8> packet = {0x81, 0xff, 0x99, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
	packet.push_back((content.size() >> 8) & 0xffu);
	packet.push_back(content.size() & 0xffu);
	std::copy(std::begin(content), std::end(content), std::back_inserter(packet));
	const auto key = config::GdxLoginKey.get();
	std::vector<u8> e_loginkey(key.size());
	static const int magic[] = {0x46, 0xcf, 0x2d, 0x55};
	for (int i = 0; i < e_loginkey.size(); ++i) e_loginkey[i] ^= key[i] ^ magic[i & 3];
	packet.push_back((e_loginkey.size() >> 8) & 0xffu);
	packet.push_back(e_loginkey.size() & 0xffu);
	std::copy(std::begin(e_loginkey), std::end(e_loginkey), std::back_inserter(packet));
	u16 payload_size = (u16)(packet.size() - 12);
	packet[4] = (payload_size >> 8) & 0xffu;
	packet[5] = payload_size & 0xffu;
	return packet;
}

std::vector<u8> Gdxsv::GenerateP2PMatchReportPacket() {
	if (rollback_net_.GetMatching().is_training_game()) {
		return {};
	}

	auto rbk_report = rollback_net_.GetReport();
	if (rbk_report.battle_code().empty()) {
		return {};
	}

	auto msg = LbsMessage::ClNotice(LbsMessage::lbsP2PMatchingReport);
	auto lines = InMemoryListener::getInstance()->getLog();

	std::ostringstream ss;
	for (const auto &line : lines) {
		ss << line;
	}
	rbk_report.set_after_log(ss.str());

	const auto& round_data = rollback_net_.GetRoundData();
	for (const auto& rd : round_data) {
		rbk_report.add_round_data()->CopyFrom(rd);
	}

	std::string data;
	if (rbk_report.SerializeToString(&data)) {
		if (!encode_zlib_deflate(data.c_str(), data.size(), msg.body)) {
			ERROR_LOG(COMMON, "report encode error");
		}
	} else {
		ERROR_LOG(COMMON, "report serialize error");
	}

	rollback_net_.ClearReport();

	if (32700 <= msg.body.size()) {
		ERROR_LOG(COMMON, "too large body");
		return {};
	}

	std::vector<u8> buf;
	msg.Serialize(buf);
	NOTICE_LOG(COMMON, "report msg size %d", buf.size());
	return buf;
}

void Gdxsv::HandleRPC() {
	u32 gdx_rpc_addr = symbols_["gdx_rpc"];
	if (gdx_rpc_addr == 0) {
		return;
	}

	u32 response = 0;
	gdx_rpc_t gdx_rpc{};
	gdx_rpc.request = gdxsv_ReadMem32(gdx_rpc_addr);
	gdx_rpc.response = gdxsv_ReadMem32(gdx_rpc_addr + 4);
	gdx_rpc.param1 = gdxsv_ReadMem32(gdx_rpc_addr + 8);
	gdx_rpc.param2 = gdxsv_ReadMem32(gdx_rpc_addr + 12);
	gdx_rpc.param3 = gdxsv_ReadMem32(gdx_rpc_addr + 16);
	gdx_rpc.param4 = gdxsv_ReadMem32(gdx_rpc_addr + 20);

	if (gdx_rpc.request == GDX_RPC_SOCK_OPEN) {
		const u32 tolobby = gdx_rpc.param1;
		const u32 host_ip = gdx_rpc.param2;
		const u16 port = gdx_rpc.param3;

		if (netmode_ == NetMode::Replay) {
			replay_net_.Open();
		} else if (tolobby == 1) {
			// WritePatch() can still be waiting to replace the payload of a
			// savestate from another version. Never go online with it.
			if (gdxsv_ReadMem32(symbols_["patch_id"]) != symbols_[":patch_id"]) {
				WARN_LOG(COMMON, "Lobby connection refused: the payload is not updated yet");
				netmode_ = NetMode::Offline;
			} else if (lbs_net_.Connect(config::GdxLobbyServer, port)) {
				netmode_ = NetMode::Lbs;
				lbs_net_.Send(GeneratePlatformInfoPacket());
			} else {
				netmode_ = NetMode::Offline;
			}
		} else {
			if (~host_ip == 0) {
				rollback_net_.Open();
				netmode_ = NetMode::McsRollback;
			} else {
				char addr_buf[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &host_ip, addr_buf, INET_ADDRSTRLEN);
				if (udp_net_.Connect(std::string(addr_buf), port)) {
					netmode_ = NetMode::McsUdp;
				} else {
					netmode_ = NetMode::Offline;
				}
			}
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_CLOSE) {
		if (netmode_ == NetMode::Lbs) {
			netmode_ = NetMode::Offline;
			if (!going_to_battle_) {
				lbs_net_.Close();
			}
			going_to_battle_ = false;
		} else if (netmode_ == NetMode::McsUdp) {
			netmode_ = NetMode::Offline;
			udp_net_.CloseMcsRemoteWithReason("cl_app_close");

		} else if (netmode_ == NetMode::McsRollback) {
			rollback_net_.SetCloseReason("cl_app_close");
			rollback_net_.Close();

			netmode_ = NetMode::Offline;
		} else if (netmode_ == NetMode::Replay) {
			replay_net_.Close();
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_READ) {
		if (netmode_ == NetMode::Lbs) {
			response = lbs_net_.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::McsUdp) {
			response = udp_net_.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::Replay) {
			response = replay_net_.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::McsRollback) {
			response = rollback_net_.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_WRITE) {
		if (netmode_ == NetMode::Lbs) {
			response = lbs_net_.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::McsUdp) {
			response = udp_net_.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::Replay) {
			response = replay_net_.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		} else if (netmode_ == NetMode::McsRollback) {
			response = rollback_net_.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_POLL) {
		if (netmode_ == NetMode::Lbs) {
			response = lbs_net_.OnSockPoll();
		} else if (netmode_ == NetMode::McsUdp) {
			response = udp_net_.OnSockPoll();
		} else if (netmode_ == NetMode::Replay) {
			response = replay_net_.OnSockPoll();
		} else if (netmode_ == NetMode::McsRollback) {
			response = rollback_net_.OnSockPoll();
		}
	}

	gdxsv_WriteMem32(gdx_rpc_addr, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 4, response);
	gdxsv_WriteMem32(gdx_rpc_addr + 8, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 12, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 16, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 20, 0);

	gdxsv_WriteMem32(symbols_["is_online"], netmode_ != NetMode::Offline);
}

void Gdxsv::StartPingTest() {
	// Reuse in-flight tests and successful results across game loads.
	if (gcp_ping_test_result_.valid() &&
		(gcp_ping_test_result_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
		 !gcp_ping_test_result_.get().empty()))
		return;

	SetPingResult("Latency check...");
	gcp_ping_test_result_ = gcp_ping_test().share();
}

void Gdxsv::StartP2PFeasibilityTest() {
	if (p2p_feasibility_result_.valid())
		return;
	if (config::GdxLocalPort != 0) {
		p2p_feasibility_result_ = test_p2p_feasibility(config::GdxLocalPort).share();
	}
}

void Gdxsv::FetchPublicIP() {
	public_ipv4_ = get_public_ip_address(false).share();
	public_ipv6_ = get_public_ip_address(true).share();
}

void Gdxsv::NotifyWanPort() const {
	if (netmode_ != NetMode::Lbs) {
		return;
	}

	const auto lbs_host = lbs_net_.RemoteHost();
	const auto lbs_port = lbs_net_.RemotePort();
	const auto udp_port = config::GdxLocalPort.get();
	const auto user_id = user_id_;

	if (lbs_host.empty() || lbs_port == 0 || udp_port == 0 || user_id.empty()) {
		return;
	}

	proto::Packet pkt;
	pkt.set_type(proto::MessageType::HelloLbs);
	pkt.mutable_hello_lbs_data()->set_user_id(user_id);
	char buf[128];
	if (!pkt.SerializePartialToArray(buf, sizeof(buf))) {
		ERROR_LOG(COMMON, "packet serialize error");
		return;
	}

	UdpClient udp;
	if (!udp.Bind(udp_port)) {
		ERROR_LOG(COMMON, "NotifyWanPort udp.Bind failed");
		return;
	}

	UdpRemote remote;
	if (!remote.Open(lbs_host.c_str(), lbs_port)) {
		ERROR_LOG(COMMON, "NotifyWanPort remote.Open failed");
		return;
	}

	udp.SendTo(buf, pkt.GetCachedSize(), remote);
	udp.Close();
}

std::string Gdxsv::GenerateLoginKey() {
	const int n = 8;
	uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	std::mt19937 gen(seed);
	std::string chars = "0123456789";
	std::uniform_int_distribution<> dist(0, chars.length() - 1);
	std::string key(n, 0);
	std::generate_n(key.begin(), n, [&]() { return chars[dist(gen)]; });
	return key;
}

void Gdxsv::ApplyOnlinePatch(bool first_time) {
	for (const auto &patch : patch_list_.patches()) {
		if (patch.write_once() && !first_time) {
			continue;
		}
		if (first_time) {
			NOTICE_LOG(COMMON, "patch apply: %s", patch.name().c_str());
		}
		for (const auto &code : patch.codes()) {
			gdxsv_WriteMem(code.size(), code.address(), code.changed());
		}
	}
}

void Gdxsv::RestoreOnlinePatch() {
	for (const auto &patch : patch_list_.patches()) {
		NOTICE_LOG(COMMON, "patch restore: %s", patch.name().c_str());
		for (const auto &code : patch.codes()) {
			gdxsv_WriteMem(code.size(), code.address(), code.original());
		}
	}
	patch_list_.clear_patches();

	// Discard dynarec cache because it may become corrupted
	emu.getSh4Executor()->ResetCache();
}

namespace {
// A savestate of another version resumes its own payload. Replacing it while
// the SH4 executes there, or will return there, continues in different code.
bool Sh4InPayloadCode() {
	const auto in_code = [](u32 addr) { return (addr & 0x1fff0000) == 0x0c4f0000; };
	if (in_code(Sh4cntx.pc) || in_code(Sh4cntx.pr))
		return true;
	const u32 sp = Sh4cntx.r[15];
	if ((sp & 0x1f000000) != 0x0c000000)
		return false;
	for (u32 i = 0; i < 64; i++)
		if (in_code(gdxsv_ReadMem32(sp + i * 4)))
			return true;
	return false;
}
} // namespace

void Gdxsv::WritePatch() {
	if (disk_ == 1) WritePatchDisk1();
	if (disk_ == 2) WritePatchDisk2();
	if (symbols_["patch_id"] == 0 || gdxsv_ReadMem32(symbols_["patch_id"]) != symbols_[":patch_id"]) {
		if (Sh4InPayloadCode()) {
			DEBUG_LOG(COMMON, "patch deferred: pc=%08x pr=%08x", Sh4cntx.pc, Sh4cntx.pr);
			return;
		}
		NOTICE_LOG(COMMON, "patch %d %d", gdxsv_ReadMem32(symbols_["patch_id"]), symbols_[":patch_id"]);
		emu.getSh4Executor()->ResetCache();

#include "gdxsv_patch.inc"

		InvalidateWidescreenPatch();
		gdxsv_WriteMem32(symbols_["disk"], (int)disk_);
		// Restore the backend flag cleared by payload initialization so guest
		// polling can reach HandleRPC without first sending another request.
		gdxsv_WriteMem32(symbols_["is_online"], netmode_ != NetMode::Offline);
	}

	if (disk_ == 2) {
		if (symbols_["lang_patch_id"] == 0 || gdxsv_ReadMem32(symbols_["lang_patch_id"]) != symbols_[":lang_patch_id"] ||
			symbols_[":lang_patch_lang"] != (u8)GdxsvLanguage::Language()) {
			NOTICE_LOG(COMMON, "lang_patch id=%d prev=%d lang=%d", gdxsv_ReadMem32(symbols_["lang_patch_id"]), symbols_[":lang_patch_id"],
					   GdxsvLanguage::Language());
#include "gdxsv_translation_patch.inc"
		}
	}
}

void Gdxsv::WritePatchDisk1() {
	const u32 offset = 0x8C000000 + 0x00010000;

	// Max Rebattle Patch
	gdxsv_WriteMem8(0x0c0345b0, maxrebattle_);

	// Fix cost 300 to 295
	gdxsv_WriteMem16(0x0c1b0fd0, 295);

	// Send key message every frame
	gdxsv_WriteMem8(0x0c310450, 1);

	// Reduce max lag-frame
	gdxsv_WriteMem8(0x0c310451, maxlag_);

	// Modem connection fix
	const char *atm1 = "ATM1\r                                ";
	for (int i = 0; i < strlen(atm1); ++i) {
		gdxsv_WriteMem8(offset + 0x0015e703 + i, u8(atm1[i]));
	}

	// Overwrite serve address (max 20 chars)
	const auto server = config::GdxLobbyServer.get();
	for (int i = 0; i < 20; ++i) {
		gdxsv_WriteMem8(offset + 0x0015e788 + i, (i < server.length()) ? u8(server[i]) : u8(0));
	}

	// Skip form validation
	gdxsv_WriteMem16(offset + 0x0003b0c4, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x0003b0cc, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x0003b0d4, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x0003b0dc, u16(9));	// nop

	// Write LoginKey
	if (gdxsv_ReadMem8(offset - 0x10000 + 0x002f6924) == 0) {
		const auto key = config::GdxLoginKey.get();
		for (int i = 0; i < std::min(key.length(), size_t(8)) + 1; ++i) {
			gdxsv_WriteMem8(offset - 0x10000 + 0x002f6924 + i, (i < key.length()) ? u8(key[i]) : u8(0));
		}
	}

	// Ally HP
	u16 hp_offset = 0x0180;
	if ((InGame() || (IsReplaying() && config::GdxReplayShowAllyHP)) && gdxsv_ReadMem8(0x0c336254) == 2 &&
		gdxsv_ReadMem8(0x0c336255) == 7) {
		u8 player_index = gdxsv_ReadMem8(0x0c2f6652);
		if (1 <= player_index && player_index <= 4) {
			player_index--;
			// depend on 4 player battle
			const u8 ally_index = player_index - (player_index & 1) + !(player_index & 1);
			const u16 ally_hp = gdxsv_ReadMem16(0x0c3369d6 + ally_index * 0x2000);
			gdxsv_WriteMem16(0x0c3369d2 + player_index * 0x2000, ally_hp);
			hp_offset -= 2;
		}
	}
	gdxsv_WriteMem16(0x0c01d336, hp_offset);
	gdxsv_WriteMem16(0x0c01d56e, hp_offset);
	gdxsv_WriteMem16(0x0c01d678, hp_offset);
	gdxsv_WriteMem16(0x0c01d89e, hp_offset);

	// Disable soft reset
	gdxsv_WriteMem8(0x0c2f6657, InGame() ? 1 : 0);

	// Online patch
	ApplyOnlinePatch(false);
}

void Gdxsv::WritePatchDisk2() {
	const u32 offset = 0x8C000000 + 0x00010000;

	// Max Rebattle Patch
	gdxsv_WriteMem8(0x0c0219ec, maxrebattle_);

	// Fix cost 300 to 295
	gdxsv_WriteMem16(0x0c21bfec, 295);
	gdxsv_WriteMem16(0x0c21bff4, 295);
	gdxsv_WriteMem16(0x0c21c034, 295);

	// Send key message every frame
	gdxsv_WriteMem8(0x0c3abb90, 1);

	// Reduce max lag-frame
	gdxsv_WriteMem8(0x0c3abb91, maxlag_);

	// Modem connection fix
	const char *atm1 = "ATM1\r                                ";
	for (int i = 0; i < strlen(atm1); ++i) {
		gdxsv_WriteMem8(offset + 0x001be7c7 + i, u8(atm1[i]));
	}

	// Overwrite server address (max 20 chars)
	const auto server = config::GdxLobbyServer.get();
	for (int i = 0; i < 20; ++i) {
		gdxsv_WriteMem8(offset + 0x001be84c + i, (i < server.length()) ? u8(server[i]) : u8(0));
	}

	// Skip form validation
	gdxsv_WriteMem16(offset + 0x000284f0, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x000284f8, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x00028500, u16(9));	// nop
	gdxsv_WriteMem16(offset + 0x00028508, u16(9));	// nop

	// Write LoginKey
	if (gdxsv_ReadMem8(offset - 0x10000 + 0x00392064) == 0) {
		const auto key = config::GdxLoginKey.get();
		for (int i = 0; i < std::min(key.length(), size_t(8)) + 1; ++i) {
			gdxsv_WriteMem8(offset - 0x10000 + 0x00392064 + i, (i < key.length()) ? u8(key[i]) : u8(0));
		}
	}

	// Ally HP
	u16 hp_offset = 0x0180;
	if ((InGame() || (IsReplaying() && config::GdxReplayShowAllyHP)) && gdxsv_ReadMem8(0x0c3d16d4) == 2 &&
		gdxsv_ReadMem8(0x0c3d16d5) == 7) {
		u8 player_index = gdxsv_ReadMem8(0x0c391d92);
		if (1 <= player_index && player_index <= 4) {
			player_index--;
			// depend on 4 player battle
			const u8 ally_index = player_index - (player_index & 1) + !(player_index & 1);
			const u16 ally_hp = gdxsv_ReadMem16(0x0c3d1e56 + ally_index * 0x2000);
			gdxsv_WriteMem16(0x0c3d1e52 + player_index * 0x2000, ally_hp);
			hp_offset -= 2;
		}
	}
	gdxsv_WriteMem16(0x0c11da88, hp_offset);
	gdxsv_WriteMem16(0x0c11dbbc, hp_offset);
	gdxsv_WriteMem16(0x0c11dcc0, hp_offset);
	gdxsv_WriteMem16(0x0c11ddd6, hp_offset);
	gdxsv_WriteMem16(0x0c11df08, hp_offset);
	gdxsv_WriteMem16(0x0c11e01a, hp_offset);

	// Disable soft reset
	gdxsv_WriteMem8(0x0c391d97, InGame() ? 1 : 0);

	// Online patch
	ApplyOnlinePatch(false);
}

namespace {
u32 FloatBits(float value) {
	u32 bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}
} // namespace

// True once every site holds its stock or its widescreen value: the game
// image is loaded and uses the known layout.
bool Gdxsv::WidescreenSitesKnown() {
	for (const auto &hook : gdxsv_abi::widescreen_hooks) {
		const u32 value = gdxsv_ReadMem32(hook.addr);
		if (hook.disk == disk_ && value != hook.stock && value != symbols_[hook.target])
			return false;
	}
	for (const auto &code : gdxsv_abi::widescreen_code) {
		const u16 value = gdxsv_ReadMem16(code.addr);
		if (code.disk == disk_ && value != code.stock && value != code.patched)
			return false;
	}
	return true;
}

void Gdxsv::WriteWidescreenPatch() {
	if (disk_ != 1 && disk_ != 2)
		return;
	const bool enabled = config::Widescreen && config::WidescreenGameHacks;
	const int width = settings.display.width;
	const int height = settings.display.height;
	if (width <= 0 || height <= 0)
		return; // A minimized window must not replace the last usable aspect.

	const bool super_widescreen = config::SuperWidescreen;
	const int hud_layout = config::GdxWidescreenHudLayout;
	// State restoration and payload installation invalidate this host cache.
	// Unchanged frames need no symbol lookups, guest reads or aspect arithmetic.
	if (enabled == widescreen_enabled_ && width == widescreen_viewport_width_ &&
		height == widescreen_viewport_height_ && super_widescreen == widescreen_super_ &&
		hud_layout == widescreen_hud_layout_)
		return;
	// A savestate can restore an older payload while host symbols stay current.
	// Wait for WritePatch() to install the current payload.
	if (symbols_.count("patch_id") == 0 || gdxsv_ReadMem32(symbols_["patch_id"]) != symbols_[":patch_id"])
		return;
	if (!WidescreenSitesKnown())
		return; // The game image may not be ready, or the layout is unsupported.

	constexpr float stock_aspect = 4.f / 3.f;
	const float viewport_aspect = static_cast<float>(width) / height;
	// With widescreen off, every value below is the stock one.
	float aspect = stock_aspect;
	float hud_aspect = stock_aspect;
	if (enabled) {
		aspect = super_widescreen ? viewport_aspect : 16.f / 9.f;
		// HUD placement follows the viewport. Full Width must stay within the
		// rendered image when SuperWidescreen is off.
		if (hud_layout == 1)
			hud_aspect = std::max(stock_aspect, std::min(viewport_aspect, 16.f / 9.f));
		else if (hud_layout == 2)
			hud_aspect = std::max(stock_aspect, std::min(viewport_aspect, aspect));
	}
	const float scale = aspect / stock_aspect;
	const float hud_offset = 0.075f * (hud_aspect - stock_aspect);
	// Include a five-pixel overscan margin for fades at fractional viewport aspects.
	const float left = 320.f - 325.f * scale;
	const float right = 320.f + 325.f * scale;

	bool changed = false;
	for (const auto &code : gdxsv_abi::widescreen_code)
		if (code.disk == disk_)
			changed |= gdxsv_WriteMem16(code.addr, enabled ? code.patched : code.stock);
	for (const auto &hook : gdxsv_abi::widescreen_hooks)
		if (hook.disk == disk_)
			changed |= gdxsv_WriteMem32(hook.addr, enabled ? symbols_[hook.target] : hook.stock);
	for (const auto &site : gdxsv_abi::widescreen_values) {
		if (site.disk != disk_)
			continue;
		u32 value = site.stock;
		if (enabled) {
			switch (site.kind) {
			case gdxsv_abi::Kind::MatteLeft: value = FloatBits(left); break;
			case gdxsv_abi::Kind::MatteRight: value = FloatBits(right); break;
			case gdxsv_abi::Kind::FrustumLeft: value = FloatBits(-0.075f * aspect); break;
			case gdxsv_abi::Kind::FrustumRight: value = FloatBits(0.075f * aspect); break;
			// The information panel's glyph loop has a private X-origin literal.
			case gdxsv_abi::Kind::InfoPanelX: value = FloatBits(-0.0457f - hud_offset * 0.5f); break;
			}
		}
		changed |= gdxsv_WriteMem32(site.addr, value);
	}
	// HUD objects which copied the entry keep calling it; stock values leave them in place.
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_transition_left_x"], FloatBits(enabled ? left : 0.f));
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_transition_right_x"], FloatBits(enabled ? right : 640.f));
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_hud_right_offset"], FloatBits(hud_offset));
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_hud_left_offset"], FloatBits(-hud_offset));
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_hud_half_left_offset"], FloatBits(-hud_offset * 0.5f));
	changed |= gdxsv_WriteMem32(symbols_["gdx_widescreen_result_black_scale"], FloatBits(enabled ? scale * 650.f / 640.f : 1.f));

	widescreen_enabled_ = enabled;
	widescreen_viewport_width_ = width;
	widescreen_viewport_height_ = height;
	widescreen_super_ = super_widescreen;
	widescreen_hud_layout_ = hud_layout;
	// Some parameters are literals in the game's code. Invalidate only when
	// hooks or parameters actually changed, never for an unchanged frame.
	if (changed) {
		emu.getSh4Executor()->ResetCache();
		NOTICE_LOG(COMMON, "widescreen patch %s: aspect=%.6f hud-aspect=%.6f disk=%d",
			enabled ? "refreshed" : "removed", aspect, hud_aspect, int(disk_));
	}
}

bool Gdxsv::StartReplayFile(const char *path, int pov) {
	replay_net_.Reset();
	const auto str = std::string(path);

	if (4 <= str.length() && str.substr(0, 4) == "http") {
		std::string content_type;
		http::init();
		std::vector<u8> downloaded;

		int rc = http::get(str, downloaded, content_type);
		if (rc != 200) {
			ERROR_LOG(COMMON, "replay download failure: %s", str.c_str());
			return false;
		}

		if (replay_net_.StartBuffer(downloaded, pov)) {
			netmode_ = NetMode::Replay;
			return true;
		}
	} else {
		if (replay_net_.StartFile(path, pov)) {
			netmode_ = NetMode::Replay;
			return true;
		}
	}

	return false;
}

bool Gdxsv::StartReplayBuffer(const std::vector<u8> &buf, int pov) {
	replay_net_.Reset();

	if (replay_net_.StartBuffer(buf, pov)) {
		netmode_ = NetMode::Replay;
		return true;
	}

	return false;
}

bool Gdxsv::StartLiveSpectate(const char *battle_code, int pov) {
	replay_net_.Reset();

	const auto host = config::GdxLobbyServer.get();
	if (replay_net_.StartLive(host, battle_code, pov)) {
		netmode_ = NetMode::Replay;
		return true;
	}

	return false;
}

void Gdxsv::StopReplay() { replay_net_.Stop(); }

bool Gdxsv::StartRollbackTest(const char *param) {
	rollback_net_.Reset();

	if (rollback_net_.StartLocalTest(param)) {
		netmode_ = NetMode::McsRollback;
		return true;
	}

	return false;
}

Gdxsv gdxsv;
