#include "gdxsv.h"

#include <sstream>
#include <random>
#include <regex>
#include <xxhash.h>

#include "lzma/CpuArch.h"
#include "oslib/oslib.h"
#include "version.h"

bool Gdxsv::InGame() const {
    return enabled && udp_net.IsConnected();
}

bool Gdxsv::Enabled() const {
    return enabled;
}

void Gdxsv::Reset() {
    lbs_net.Reset();
    udp_net.Reset();
    RestoreOnlinePatch();

    // Automatically add ContentPath if it is empty.
    if (config::ContentPath.get().empty()) {
        config::ContentPath.get().push_back("./");
    }

    auto game_id = std::string(ip_meta.product_number, sizeof(ip_meta.product_number));
    if (game_id != "T13306M   ") {
        enabled = false;
        return;
    }
    enabled = true;

#ifdef __APPLE__
    signal(SIGPIPE, SIG_IGN);
#endif

    lbs_net.callback_lbs_packet([this](const LbsMessage &lbs_msg) {
        if (lbs_msg.command == LbsMessage::lbsExtPlayerInfo) {
            proto::ExtPlayerInfo player_info;
            if (player_info.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
                ext_player_info.push_back(player_info);
            }
        }
        if (lbs_msg.command == LbsMessage::lbsReadyBattle) {
            // Reset current patches for no-patched game
            RestoreOnlinePatch();
            ext_player_info.clear();
        }
        if (lbs_msg.command == LbsMessage::lbsGamePatch) {
            // Reset current patches and update patch_list
            RestoreOnlinePatch();
            if (patch_list.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
                ApplyOnlinePatch(true);
            } else {
                ERROR_LOG(COMMON, "patch_list deserialize error");
            }
        }
    });

    std::thread([this]() { GcpPingTest(); }).detach();

    server = cfgLoadStr("gdxsv", "server", "zdxsv.net");
    loginkey = cfgLoadStr("gdxsv", "loginkey", "");

    if (loginkey.empty()) {
        loginkey = GenerateLoginKey();
    }

    cfgSaveStr("gdxsv", "server", server.c_str());
    cfgSaveStr("gdxsv", "loginkey", loginkey.c_str());

    std::string disk_num(ip_meta.disk_num, 1);
    if (disk_num == "1") disk = 1;
    if (disk_num == "2") disk = 2;

    NOTICE_LOG(COMMON, "gdxsv disk:%d server:%s loginkey:%s", (int) disk, server.c_str(), loginkey.c_str());
}

void Gdxsv::Update() {
    if (!enabled) return;
    WritePatch();

    u8 dump_buf[1024];
    if (ReadMem32_nommu(symbols["print_buf_pos"])) {
        int n = ReadMem32_nommu(symbols["print_buf_pos"]);
        n = std::min(n, (int) sizeof(dump_buf));
        for (int i = 0; i < n; i++) {
            dump_buf[i] = ReadMem8_nommu(symbols["print_buf"] + i);
        }
        dump_buf[n] = 0;
        WriteMem32_nommu(symbols["print_buf_pos"], 0);
        WriteMem32_nommu(symbols["print_buf"], 0);
        NOTICE_LOG(COMMON, "%s", dump_buf);
    }
}

std::string Gdxsv::GeneratePlatformInfoString() {
    std::stringstream ss;
    ss << "flycast=" << GIT_VERSION << "\n";
    ss << "git_hash=" << GIT_HASH << "\n";
    ss << "build_date=" << BUILD_DATE << "\n";
    ss << "cpu=" <<
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
    ss << "os=" <<
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
    ss << "disk=" << (int) disk << "\n";
    ss << "patch_id=" << symbols[":patch_id"] << "\n";
    std::string machine_id = os_GetMachineID();
    if (machine_id.length()) {
        auto digest = XXH64(machine_id.c_str(), machine_id.size(), 37);
        ss << "machine_id=" << std::hex << digest << std::dec << "\n";
    }
    ss << "wireless=" << (int) (os_GetConnectionMedium() == "Wireless") << "\n";
    ss << "local_ip=" << lbs_net.LocalIP() << "\n";
    // ss << "bind_port=" << .bind_port() << "\n";

    if (gcp_ping_test_finished) {
        for (const auto &res : gcp_ping_test_result) {
            ss << res.first << "=" << res.second << "\n";
        }
    }
    return ss.str();
}

std::vector<u8> Gdxsv::GeneratePlatformInfoPacket() {
    std::vector<u8> packet = {0x81, 0xff, 0x99, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
    auto s = GeneratePlatformInfoString();
    packet.push_back((s.size() >> 8) & 0xffu);
    packet.push_back(s.size() & 0xffu);
    std::copy(std::begin(s), std::end(s), std::back_inserter(packet));
    std::vector<u8> e_loginkey(loginkey.size());
    static const int magic[] = {0x46, 0xcf, 0x2d, 0x55};
    for (int i = 0; i < e_loginkey.size(); ++i) e_loginkey[i] ^= loginkey[i] ^ magic[i & 3];
    packet.push_back((e_loginkey.size() >> 8) & 0xffu);
    packet.push_back(e_loginkey.size() & 0xffu);
    std::copy(std::begin(e_loginkey), std::end(e_loginkey), std::back_inserter(packet));
    u16 payload_size = (u16) (packet.size() - 12);
    packet[4] = (payload_size >> 8) & 0xffu;
    packet[5] = payload_size & 0xffu;
    return packet;
}

void Gdxsv::SyncNetwork(bool write) {
    gdx_rpc_t gdx_rpc{};
    u32 gdx_rpc_addr = symbols["gdx_rpc"];
    if (gdx_rpc_addr == 0) {
        return;
    }

    gdx_rpc.request = ReadMem32_nommu(gdx_rpc_addr);
    if (gdx_rpc.request) {
        gdx_rpc.response = ReadMem32_nommu(gdx_rpc_addr + 4);
        gdx_rpc.param1 = ReadMem32_nommu(gdx_rpc_addr + 8);
        gdx_rpc.param2 = ReadMem32_nommu(gdx_rpc_addr + 12);
        gdx_rpc.param3 = ReadMem32_nommu(gdx_rpc_addr + 16);
        gdx_rpc.param4 = ReadMem32_nommu(gdx_rpc_addr + 20);

        if (gdx_rpc.request == GDXRPC_TCP_OPEN) {
            u32 tolobby = gdx_rpc.param1;
            u32 host_ip = gdx_rpc.param2;
            u32 port_no = gdx_rpc.param3;

            std::string host = server;
            u16 port = port_no;

            if (tolobby == 1) {
                udp_net.CloseMcsRemoteWithReason("cl_to_lobby");
                if (lbs_net.Connect(host, port)) {
                    netmode = NetMode::Lbs;
                    auto packet = GeneratePlatformInfoPacket();
                    lbs_net.Send(packet);
                } else {
                    netmode = NetMode::Offline;
                }
            } else {
                lbs_net.Close();
                char addr_buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &host_ip, addr_buf, INET_ADDRSTRLEN);
                host = std::string(addr_buf);
                if (udp_net.Connect(host, port)) {
                    netmode = NetMode::McsUdp;
                } else {
                    netmode = NetMode::Offline;
                }
            }
        }

        if (gdx_rpc.request == GDXRPC_TCP_CLOSE) {
            lbs_net.Close();

            if (gdx_rpc.param2 == 0) {
                udp_net.CloseMcsRemoteWithReason("cl_app_close");
            } else if (gdx_rpc.param2 == 1) {
                udp_net.CloseMcsRemoteWithReason("cl_ppp_close");
            } else if (gdx_rpc.param2 == 2) {
                udp_net.CloseMcsRemoteWithReason("cl_soft_reset");
            } else {
                udp_net.CloseMcsRemoteWithReason("cl_tcp_close");
            }

            netmode = NetMode::Offline;
        }

        WriteMem32_nommu(gdx_rpc_addr, 0);
        WriteMem32_nommu(gdx_rpc_addr + 4, 0);
        WriteMem32_nommu(gdx_rpc_addr + 8, 0);
        WriteMem32_nommu(gdx_rpc_addr + 12, 0);
        WriteMem32_nommu(gdx_rpc_addr + 16, 0);
        WriteMem32_nommu(gdx_rpc_addr + 20, 0);
    }

    WriteMem32_nommu(symbols["is_online"], netmode != NetMode::Offline);

    switch (netmode) {
        case NetMode::Offline:
            break;

        case NetMode::Lbs:
            if (write) {
                lbs_net.OnGameWrite();
            } else {
                lbs_net.OnGameRead();
            }
            break;

        case NetMode::McsUdp:
            if (write) {
                udp_net.OnGameWrite();
            } else {
                udp_net.OnGameRead();
            }
            break;
    }

}

void Gdxsv::GcpPingTest() {
    // powered by https://github.com/cloudharmony/network
    static const std::string get_path = "/probe/ping.js";
    static const std::map<std::string, std::string> gcp_region_hosts = {
            {"asia-east1",              "asia-east1-gce.cloudharmony.net"},
            {"asia-east2",              "asia-east2-gce.cloudharmony.net"},
            {"asia-northeast1",         "asia-northeast1-gce.cloudharmony.net"},
            {"asia-northeast2",         "asia-northeast2-gce.cloudharmony.net"},
            {"asia-northeast3",         "asia-northeast3-gce.cloudharmony.net"},
            // {"asia-south1",             "asia-south1-gce.cloudharmony.net"}, // inactive now.
            {"asia-southeast1",         "asia-southeast1-gce.cloudharmony.net"},
            {"australia-southeast1",    "australia-southeast1-gce.cloudharmony.net"},
            {"europe-north1",           "europe-north1-gce.cloudharmony.net"},
            {"europe-west1",            "europe-west1-gce.cloudharmony.net"},
            {"europe-west2",            "europe-west2-gce.cloudharmony.net"},
            {"europe-west3",            "europe-west3-gce.cloudharmony.net"},
            {"europe-west4",            "europe-west4-gce.cloudharmony.net"},
            {"europe-west6",            "europe-west6-gce.cloudharmony.net"},
            {"northamerica-northeast1", "northamerica-northeast1-gce.cloudharmony.net"},
            {"southamerica-east1",      "southamerica-east1-gce.cloudharmony.net"},
            {"us-central1",             "us-central1-gce.cloudharmony.net"},
            {"us-east1",                "us-east1-gce.cloudharmony.net"},
            {"us-east4",                "us-east4-gce.cloudharmony.net"},
            {"us-west1",                "us-west1-gce.cloudharmony.net"},
            {"us-west2",                "us-west2-a-gce.cloudharmony.net"},
            {"us-west3",                "us-west3-gce.cloudharmony.net"},
    };

    for (const auto &region_host : gcp_region_hosts) {
        TcpClient client;
        std::stringstream ss;
        ss << "HEAD " << get_path << " HTTP/1.1" << "\r\n";
        ss << "Host: " << region_host.second << "\r\n";
        ss << "User-Agent: flycast for gdxsv" << "\r\n";
        ss << "Accept: */*" << "\r\n";
        ss << "\r\n"; // end of header

        if (!client.Connect(region_host.second.c_str(), 80)) {
            ERROR_LOG(COMMON, "connect failed : %s", region_host.first.c_str());
            continue;
        }

        auto request_header = ss.str();
        auto t1 = std::chrono::high_resolution_clock::now();
        int n = client.Send(request_header.c_str(), request_header.size());
        if (n < request_header.size()) {
            ERROR_LOG(COMMON, "send failed : %s", region_host.first.c_str());
            client.Close();
            continue;
        }

        char buf[1024] = {0};
        n = client.Recv(buf, 1024);
        if (n <= 0) {
            ERROR_LOG(COMMON, "recv failed : %s", region_host.first.c_str());
            client.Close();
            continue;
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        int rtt = (int) std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        const std::string response_header(buf, n);
        if (response_header.find("200 OK") != std::string::npos) {
            gcp_ping_test_result[region_host.first] = rtt;
            char latency_str[256];
            snprintf(latency_str, 256, "%s : %d[ms]", region_host.first.c_str(), rtt);
            NOTICE_LOG(COMMON, "%s", latency_str);
            gui_display_notification(latency_str, 3000);
        } else {
            ERROR_LOG(COMMON, "error response : %s", response_header.c_str());
        }
        client.Close();
    }
    gcp_ping_test_finished = true;
    gui_display_notification("Google Cloud latency checked!", 3000);
}

std::string Gdxsv::GenerateLoginKey() {
    const int n = 8;
    uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937 gen(seed);
    std::string chars = "0123456789";
    std::uniform_int_distribution<> dist(0, chars.length() - 1);
    std::string key(n, 0);
    std::generate_n(key.begin(), n, [&]() {
        return chars[dist(gen)];
    });
    return key;
}

void Gdxsv::ApplyOnlinePatch(bool first_time) {
    for (int i = 0; i < patch_list.patches_size(); ++i) {
        auto &patch = patch_list.patches(i);
        if (patch.write_once() && !first_time) {
            continue;
        }
        if (first_time) {
            NOTICE_LOG(COMMON, "patch apply: %s", patch.name().c_str());
        }
        for (int j = 0; j < patch.codes_size(); ++j) {
            auto &code = patch.codes(j);
            if (code.size() == 8) {
                WriteMem8_nommu(code.address(), (u8) (code.changed() & 0xff));
            }
            if (code.size() == 16) {
                WriteMem16_nommu(code.address(), (u16) (code.changed() & 0xffff));
            }
            if (code.size() == 32) {
                WriteMem32_nommu(code.address(), code.changed());
            }
        }
    }
}

void Gdxsv::RestoreOnlinePatch() {
    for (int i = 0; i < patch_list.patches_size(); ++i) {
        auto &patch = patch_list.patches(i);
        NOTICE_LOG(COMMON, "patch restore: %s", patch.name().c_str());
        for (int j = 0; j < patch.codes_size(); ++j) {
            auto &code = patch.codes(j);
            if (code.size() == 8) {
                WriteMem8_nommu(code.address(), (u8) (code.original() & 0xff));
            }
            if (code.size() == 16) {
                WriteMem16_nommu(code.address(), (u16) (code.original() & 0xffff));
            }
            if (code.size() == 32) {
                WriteMem32_nommu(code.address(), code.original());
            }
        }
    }
    patch_list.clear_patches();
}

void Gdxsv::WritePatch() {
    if (disk == 1) WritePatchDisk1();
    if (disk == 2) WritePatchDisk2();
    if (symbols["patch_id"] == 0 || ReadMem32_nommu(symbols["patch_id"]) != symbols[":patch_id"]) {
        NOTICE_LOG(COMMON, "patch %d %d", ReadMem32_nommu(symbols["patch_id"]), symbols[":patch_id"]);

#include "gdxsv_patch.h"

        WriteMem32_nommu(symbols["disk"], (int) disk);
    }
}

void Gdxsv::WritePatchDisk1() {
    const u32 offset = 0x8C000000 + 0x00010000;

    // Max Rebattle Patch
    WriteMem8_nommu(0x0c0345b0, 5);

    // Fix cost 300 to 295
    WriteMem16_nommu(0x0c1b0fd0, 295);

    // Reduce max lag-frame
    WriteMem8_nommu(0x0c310451, maxlag);

    // Modem connection fix
    const char *atm1 = "ATM1\r                                ";
    for (int i = 0; i < strlen(atm1); ++i) {
        WriteMem8_nommu(offset + 0x0015e703 + i, u8(atm1[i]));
    }

    // Overwrite serve address (max 20 chars)
    for (int i = 0; i < 20; ++i) {
        WriteMem8_nommu(offset + 0x0015e788 + i, (i < server.length()) ? u8(server[i]) : u8(0));
    }

    // Skip form validation
    WriteMem16_nommu(offset + 0x0003b0c4, u16(9)); // nop
    WriteMem16_nommu(offset + 0x0003b0cc, u16(9)); // nop
    WriteMem16_nommu(offset + 0x0003b0d4, u16(9)); // nop
    WriteMem16_nommu(offset + 0x0003b0dc, u16(9)); // nop

    // Write LoginKey
    if (ReadMem8_nommu(offset - 0x10000 + 0x002f6924) == 0) {
        for (int i = 0; i < std::min(loginkey.length(), size_t(8)) + 1; ++i) {
            WriteMem8_nommu(offset - 0x10000 + 0x002f6924 + i,
                            (i < loginkey.length()) ? u8(loginkey[i]) : u8(0));
        }
    }

    // Ally HP
    u16 hp_offset = 0x0180;
    if (InGame()) {
        u8 player_index = ReadMem8_nommu(0x0c2f6652);
        if (player_index) {
            player_index--;
            // depend on 4 player battle
            u8 ally_index = player_index - (player_index & 1) + !(player_index & 1);
            u16 ally_hp = ReadMem16_nommu(0x0c3369d6 + ally_index * 0x2000);
            WriteMem16_nommu(0x0c3369d2 + player_index * 0x2000, ally_hp);
        }
        hp_offset -= 2;
    }
    WriteMem16_nommu(0x0c01d336, hp_offset);
    WriteMem16_nommu(0x0c01d56e, hp_offset);
    WriteMem16_nommu(0x0c01d678, hp_offset);
    WriteMem16_nommu(0x0c01d89e, hp_offset);

    // Disable soft reset
    WriteMem8_nommu(0x0c2f6657, InGame() ? 1 : 0);

    // Online patch
    ApplyOnlinePatch(false);
}

void Gdxsv::WritePatchDisk2() {
    const u32 offset = 0x8C000000 + 0x00010000;

    // Max Rebattle Patch
    WriteMem8_nommu(0x0c0219ec, 5);

    // Fix cost 300 to 295
    WriteMem16_nommu(0x0c21bfec, 295);
    WriteMem16_nommu(0x0c21bff4, 295);
    WriteMem16_nommu(0x0c21c034, 295);

    // Reduce max lag-frame
    // WriteMem8_nommu(offset + 0x00035348, maxlag);
    // WriteMem8_nommu(offset + 0x0003534e, maxlag);
    WriteMem8_nommu(0x0c3abb91, maxlag);

    // Modem connection fix
    const char *atm1 = "ATM1\r                                ";
    for (int i = 0; i < strlen(atm1); ++i) {
        WriteMem8_nommu(offset + 0x001be7c7 + i, u8(atm1[i]));
    }

    // Overwrite serve address (max 20 chars)
    for (int i = 0; i < 20; ++i) {
        WriteMem8_nommu(offset + 0x001be84c + i, (i < server.length()) ? u8(server[i]) : u8(0));
    }

    // Skip form validation
    WriteMem16_nommu(offset + 0x000284f0, u16(9)); // nop
    WriteMem16_nommu(offset + 0x000284f8, u16(9)); // nop
    WriteMem16_nommu(offset + 0x00028500, u16(9)); // nop
    WriteMem16_nommu(offset + 0x00028508, u16(9)); // nop

    // Write LoginKey
    if (ReadMem8_nommu(offset - 0x10000 + 0x00392064) == 0) {
        for (int i = 0; i < std::min(loginkey.length(), size_t(8)) + 1; ++i) {
            WriteMem8_nommu(offset - 0x10000 + 0x00392064 + i,
                            (i < loginkey.length()) ? u8(loginkey[i]) : u8(0));
        }
    }

    // Ally HP
    u16 hp_offset = 0x0180;
    if (InGame()) {
        u8 player_index = ReadMem8_nommu(0x0c391d92);
        if (player_index) {
            player_index--;
            // depend on 4 player battle
            u8 ally_index = player_index - (player_index & 1) + !(player_index & 1);
            u16 ally_hp = ReadMem16_nommu(0x0c3d1e56 + ally_index * 0x2000);
            WriteMem16_nommu(0x0c3d1e52 + player_index * 0x2000, ally_hp);
        }
        hp_offset -= 2;
    }
    WriteMem16_nommu(0x0c11da88, hp_offset);
    WriteMem16_nommu(0x0c11dbbc, hp_offset);
    WriteMem16_nommu(0x0c11dcc0, hp_offset);
    WriteMem16_nommu(0x0c11ddd6, hp_offset);
    WriteMem16_nommu(0x0c11df08, hp_offset);
    WriteMem16_nommu(0x0c11e01a, hp_offset);

    // Disable soft reset
    WriteMem8_nommu(0x0c391d97, InGame() ? 1 : 0);

    // Online patch
    ApplyOnlinePatch(false);

    // Dirty widescreen cheat
    if (config::WidescreenGameHacks.get()) {
        u32 ratio = 0x3faaaaab; // default 4/3
        int stretching = 100;
        bool update = false;
        if (ReadMem8_nommu(0x0c3d16d4) == 2 && ReadMem8_nommu(0x0c3d16d5) == 7) { // In main game part
            // Changing this value outside the game part will break UI layout.
            // For 0x0c3d16d5: 4=load briefing, 5=briefing, 7=battle, 0xd=rebattle/end selection
            if (config::ScreenStretching == 100) {
                // ratio = 0x3fe4b17e; // wide 4/3 * 1.34
                // stretching = 134;
                // Use a little wider than 16/9 because of a glitch at the edges of the screen.
                ratio = 0x40155555;
                stretching = 175;
                update = true;
            }
        } else {
            if (config::ScreenStretching != 100) {
                update = true;
            }
        }
        if (update) {
            config::ScreenStretching.override(stretching);
            WriteMem32_nommu(0x0c1e7948, ratio);
            WriteMem32_nommu(0x0c1e7958, ratio);
            WriteMem32_nommu(0x0c1e7968, ratio);
            WriteMem32_nommu(0x0c1e7978, ratio);
        }
    }
}

void Gdxsv::handleReleaseJSON(const std::string &json) {
    const std::string version_prefix = "gdxsv-";
    const std::regex tag_name_regex(R"|#|("tag_name":"(.*?)")|#|");
    const std::regex semver_regex(
            R"|#|(^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+)?$)|#|");

    auto trim_prefix = [&version_prefix](const std::string &s) {
        if (s.size() < version_prefix.size())
            return s;
        if (version_prefix == s.substr(0, version_prefix.size()))
            return s.substr(version_prefix.size());
        return s;
    };

    std::smatch match;

    auto current_version_str = trim_prefix(std::string(GIT_VERSION));
    if (!std::regex_match(current_version_str, match, semver_regex)) return;

    if (match.size() < 4) return;
    auto current_version = std::tuple<int, int, int>(
            std::stoi(match.str(1)), std::stoi(match.str(2)), std::stoi(match.str(3)));

    if (!std::regex_search(json, match, tag_name_regex)) return;
    if (match.size() < 2) return;
    auto tag_name = match.str(1);
    auto latest_version_str = trim_prefix(tag_name);

    if (!std::regex_match(latest_version_str, match, semver_regex)) return;
    if (match.size() < 4) return;
    auto latest_version = std::tuple<int, int, int>(
            std::stoi(match.str(1)), std::stoi(match.str(2)), std::stoi(match.str(3)));

    latest_version_tag = tag_name;

    if (current_version < latest_version) {
        update_available = true;
    }
}

bool Gdxsv::UpdateAvailable() {
    static std::once_flag once;
    std::call_once(once, [this] {
        std::thread([this]() {
            const std::string json = os_FetchStringFromURL(
                    "https://api.github.com/repos/inada-s/flycast/releases/latest");
            if (json.empty()) return;
            handleReleaseJSON(json);
        }).detach();
    });
    return update_available;
}

void Gdxsv::OpenDownloadPage() {
    os_LaunchFromURL("https://github.com/inada-s/flycast/releases/latest/");
    update_available = false;
}

void Gdxsv::DismissUpdateDialog() {
    update_available = false;
}

std::string Gdxsv::LatestVersion() {
    return latest_version_tag;
}

Gdxsv gdxsv;
