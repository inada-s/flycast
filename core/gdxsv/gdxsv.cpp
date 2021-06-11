#include "gdxsv.h"

#include <sstream>
#include <random>
#include <regex>
#include <iomanip>

#include "packet.pb.h"
#include "battlelog.pb.h"
#include "gdx_queue.h"
#include "lbs_message.h"
#include "mcs_message.h"

#include "version.h"
#include "rend/gui.h"
#include "oslib/oslib.h"
#include "lzma/CpuArch.h"
#include "deps/crypto/sha1.h"

extern void dc_stop();

extern void dc_loadstate();

extern void dc_resume();

extern bool dc_is_load_done();

Gdxsv::~Gdxsv() {
    tcp_client.Close();
    net_terminate = true;
    if (net_thread.joinable()) {
        net_thread.join();
    }
    CloseUdpClientWithReason("cl_hard_quit");
}

bool Gdxsv::InGame() const {
    return enabled && udp_client.IsConnected();
}

bool Gdxsv::Enabled() const {
    return enabled;
}

void Gdxsv::Reset() {
    NOTICE_LOG(COMMON, "RESET__");
    if (settings.dreamcast.ContentPath.empty()) {
        settings.dreamcast.ContentPath.emplace_back("./");
    }

    tcp_client.Close();
    CloseUdpClientWithReason("cl_hard_reset");

    auto game_id = std::string(ip_meta.product_number, sizeof(ip_meta.product_number));
    if (game_id != "T13306M   ") {
        enabled = false;
        return;
    }
    enabled = true;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0) {
        ERROR_LOG(COMMON, "WSAStartup failed. errno=%d", get_last_error());
        return;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    if (!net_thread.joinable()) {
        NOTICE_LOG(COMMON, "start net thread");
        net_thread = std::thread([this]() {
            UpdateNetwork();
            NOTICE_LOG(COMMON, "end net thread");
        });
    }

    server = cfgLoadStr("gdxsv", "server", "zdxsv.net");
    maxlag = cfgLoadInt("gdxsv", "maxlag", 8); // Note: This should be not configurable. This is for development.
    loginkey = cfgLoadStr("gdxsv", "loginkey", "");
    bool overwriteconf = cfgLoadBool("gdxsv", "overwriteconf", true);

    if (loginkey.empty()) {
        loginkey = GenerateLoginKey();
    }

    cfgSaveStr("gdxsv", "server", server.c_str());
    cfgSaveStr("gdxsv", "loginkey", loginkey.c_str());
    cfgSaveBool("gdxsv", "overwriteconf", overwriteconf);

    std::string disk_num(ip_meta.disk_num, 1);
    if (disk_num == "1") disk = 1;
    if (disk_num == "2") disk = 2;
    NOTICE_LOG(COMMON, "gdxsv disk:%d server:%s loginkey:%s maxlag:%d", (int) disk, server.c_str(), loginkey.c_str(),
               (int) maxlag);
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

    StartReplay();
}

std::string Gdxsv::GeneratePlatformInfoString() {
    std::stringstream ss;
    ss << "flycast=" << REICAST_VERSION << "\n";
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
       #elif HOST_OS == OS_LINUX
       "Linux"
       #elif defined(__APPLE__)
       #ifdef TARGET_IPHONE
       "iOS"
       #else
       "OSX"
       #endif
       #elif defined(_WIN32)
       "Windows"
       #else
       "Unknown"
       #endif
       << "\n";
    ss << "disk=" << (int) disk << "\n";
    ss << "maxlag=" << (int) maxlag << "\n";
    ss << "patch_id=" << symbols[":patch_id"] << "\n";
    std::string machine_id = os_GetMachineID();
    if(machine_id.length()){
        sha1_ctx shactx;
        sha1_init(&shactx);
        sha1_update(&shactx, (uint32_t)machine_id.length(), reinterpret_cast<const UINT8 *>(machine_id.c_str()));
        sha1_final(&shactx);
        ss << "machine_id=" << std::hex << std::setfill('0') << std::setw(8) << shactx.digest[0] << std::setw(8) <<  shactx.digest[1] << std::setw(8) << shactx.digest[2] << std::setw(8) << shactx.digest[3] << std::setw(8) << shactx.digest[4] << std::dec << "\n";
    }
    ss << "wireless=" << (int) (os_GetConnectionMedium() == "Wireless") << "\n";
    
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
    if (write) {
        gdx_queue q;
        u32 gdx_txq_addr = symbols["gdx_txq"];
        if (gdx_txq_addr == 0) return;
        u32 buf_addr = gdx_txq_addr + 4;
        q.head = ReadMem16_nommu(gdx_txq_addr);
        q.tail = ReadMem16_nommu(gdx_txq_addr + 2);
        int n = gdx_queue_size(&q);
        if (0 < n) {
            send_buf_mtx.lock();
            for (int i = 0; i < n; ++i) {
                send_buf.push_back(ReadMem8_nommu(buf_addr + q.head));
                gdx_queue_pop(&q);
            }
            send_buf_mtx.unlock();
            WriteMem16_nommu(gdx_txq_addr, q.head);
        }
    } else {
        gdx_rpc_t gdx_rpc;
        u32 gdx_rpc_addr = symbols["gdx_rpc"];
        if (gdx_rpc_addr == 0) return;
        gdx_rpc.request = ReadMem32_nommu(gdx_rpc_addr);
        if (gdx_rpc.request) {
            gdx_rpc.response = ReadMem32_nommu(gdx_rpc_addr + 4);
            gdx_rpc.param1 = ReadMem32_nommu(gdx_rpc_addr + 8);
            gdx_rpc.param2 = ReadMem32_nommu(gdx_rpc_addr + 12);
            gdx_rpc.param3 = ReadMem32_nommu(gdx_rpc_addr + 16);
            gdx_rpc.param4 = ReadMem32_nommu(gdx_rpc_addr + 20);

            if (gdx_rpc.request == GDXRPC_TCP_OPEN) {
                recv_buf_mtx.lock();
                recv_buf.clear();
                recv_buf_mtx.unlock();

                send_buf_mtx.lock();
                send_buf.clear();
                send_buf_mtx.unlock();

                u32 tolobby = gdx_rpc.param1;
                u32 host_ip = gdx_rpc.param2;
                u32 port_no = gdx_rpc.param3;

                std::string host = server;
                u16 port = port_no;

                if (replay_mode) {
                    replay_state++;
                } else if (tolobby == 1) {
                    CloseUdpClientWithReason("cl_to_lobby");
                    bool ok = tcp_client.Connect(host.c_str(), port);
                    if (ok) {
                        tcp_client.SetNonBlocking();
                        auto packet = GeneratePlatformInfoPacket();
                        send_buf_mtx.lock();
                        send_buf.clear();
                        std::copy(begin(packet), end(packet), std::back_inserter(send_buf));
                        send_buf_mtx.unlock();
                    } else {
                        WARN_LOG(COMMON, "Failed to connect with TCP %s:%d", host.c_str(), port);
                    }
                } else {
                    tcp_client.Close();
                    char addr_buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &host_ip, addr_buf, INET_ADDRSTRLEN);
                    host = std::string(addr_buf);
                    bool ok = udp_client.Connect(host.c_str(), port);
                    if (ok) {
                        send_buf_mtx.lock();
                        send_buf.clear();
                        send_buf_mtx.unlock();

                        start_udp_session = true;
                        recv_buf_mtx.lock();
                        recv_buf.clear();
                        recv_buf_mtx.unlock();
                    } else {
                        WARN_LOG(COMMON, "Failed to connect with UDP %s:%d", host.c_str(), port);
                        ok = tcp_client.Connect(host.c_str(), port);
                        if (ok) {
                            tcp_client.SetNonBlocking();

                            send_buf_mtx.lock();
                            send_buf.clear();
                            send_buf_mtx.unlock();

                            recv_buf_mtx.lock();
                            recv_buf.clear();
                            recv_buf_mtx.unlock();
                        } else {
                            WARN_LOG(COMMON, "Failed to connect with TCP %s:%d", host.c_str(), port);
                        }
                    }
                }
            }

            if (gdx_rpc.request == GDXRPC_TCP_CLOSE) {
                tcp_client.Close();

                if (gdx_rpc.param2 == 0) {
                    CloseUdpClientWithReason("cl_app_close");
                } else if (gdx_rpc.param2 == 1) {
                    CloseUdpClientWithReason("cl_ppp_close");
                } else if (gdx_rpc.param2 == 2) {
                    CloseUdpClientWithReason("cl_soft_reset");
                } else {
                    CloseUdpClientWithReason("cl_tcp_close");
                }

                recv_buf_mtx.lock();
                recv_buf.clear();
                recv_buf_mtx.unlock();

                send_buf_mtx.lock();
                send_buf.clear();
                send_buf_mtx.unlock();
            }

            WriteMem32_nommu(gdx_rpc_addr, 0);
            WriteMem32_nommu(gdx_rpc_addr + 4, 0);
            WriteMem32_nommu(gdx_rpc_addr + 8, 0);
            WriteMem32_nommu(gdx_rpc_addr + 12, 0);
            WriteMem32_nommu(gdx_rpc_addr + 16, 0);
            WriteMem32_nommu(gdx_rpc_addr + 20, 0);
        }

        WriteMem32_nommu(symbols["is_online"],
                         tcp_client.IsConnected() || udp_client.IsConnected() || replay_state != 0);

        recv_buf_mtx.lock();
        int n = recv_buf.size();
        recv_buf_mtx.unlock();
        if (0 < n) {
            gdx_queue q;
            u32 gdx_rxq_addr = symbols["gdx_rxq"];
            u32 buf_addr = gdx_rxq_addr + 4;
            q.head = ReadMem16_nommu(gdx_rxq_addr);
            q.tail = ReadMem16_nommu(gdx_rxq_addr + 2);

            u8 buf[GDX_QUEUE_SIZE];
            recv_buf_mtx.lock();
            n = std::min<int>(recv_buf.size(), gdx_queue_avail(&q));
            for (int i = 0; i < n; ++i) {
                WriteMem8_nommu(buf_addr + q.tail, recv_buf.front());
                recv_buf.pop_front();
                gdx_queue_push(&q, 0);
            }
            recv_buf_mtx.unlock();
            WriteMem16_nommu(gdx_rxq_addr + 2, q.tail);
        }
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

void Gdxsv::UpdateNetwork() {
    // GcpPingTest();
    static const int kFirstMessageSize = 20;

    MessageBuffer message_buf;
    MessageFilter message_filter;
    proto::Packet pkt;
    bool updated = false;
    int udp_retransmit_countdown = 0;
    u8 buf[16 * 1024];

    auto mcs_ping_test = [&]() {
        if (!udp_client.IsConnected()) return;
        int ping_cnt = 0;
        int rtt_sum = 0;

        for (int i = 0; i < 10; ++i) {
            pkt.Clear();
            pkt.set_type(proto::MessageType::Ping);
            pkt.mutable_ping_data()->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count());
            if (pkt.SerializePartialToArray((void *) buf, (int) sizeof(buf))) {
                udp_client.Send((const char *) buf, pkt.GetCachedSize());
            } else {
                ERROR_LOG(COMMON, "packet serialize error");
                return;
            }

            u8 buf2[1024];
            proto::Packet pkt2;
            for (int j = 0; j < 100; ++j) {
                if (!udp_client.IsConnected()) break;
                int n = udp_client.Recv((char *) buf2, sizeof(buf2));
                if (0 < n) {
                    auto t2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                    if (pkt2.ParseFromArray(buf2, n)) {
                        if (pkt2.type() == proto::MessageType::Pong) {
                            auto t1_ = pkt2.pong_data().timestamp();
                            auto ms = t2 - t1_;
                            NOTICE_LOG(COMMON, "PING %d ms", ms);
                            ping_cnt++;
                            rtt_sum += ms;
                            break;
                        }
                    } else {
                        ERROR_LOG(COMMON, "packet deserialize error");
                        return;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        auto rtt = double(rtt_sum) / ping_cnt;
        NOTICE_LOG(COMMON, "PING AVG %.2f ms", rtt);
        maxlag = std::min<int>(0x7f, std::max(5, 4 + (int) std::floor(rtt / 16)));
        NOTICE_LOG(COMMON, "set maxlag %d", (int) maxlag);

        char osd_msg[128] = {};
        sprintf(osd_msg, "PING:%.0fms DELAY:%dfr", rtt, (int) maxlag);
        gui_display_notification(osd_msg, 3000);
    };

    while (!net_terminate) {
        if (!updated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        updated = false;
        if (!tcp_client.IsConnected() && !udp_client.IsConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (replay_mode == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        if (start_udp_session) {
            start_udp_session = false;
            udp_retransmit_countdown = 0;
            user_id.clear();
            session_id.clear();
            message_buf.Clear();
            message_filter.Clear();
            bool udp_session_ok = false;

            mcs_ping_test();

            // get session_id from client
            recv_buf_mtx.lock();
            recv_buf.assign({0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd});
            recv_buf_mtx.unlock();
            for (int i = 0; i < 60; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                send_buf_mtx.lock();
                int n = send_buf.size();
                send_buf_mtx.unlock();
                if (n < kFirstMessageSize) {
                    continue;
                }

                send_buf_mtx.lock();
                for (int j = 12; j < kFirstMessageSize; ++j) {
                    session_id.push_back((char) send_buf[j]);
                }
                send_buf_mtx.unlock();
                break;
            }

            NOTICE_LOG(COMMON, "session_id:%s", session_id.c_str());

            // send session_id to server
            if (!session_id.empty()) {
                pkt.Clear();
                pkt.set_type(proto::MessageType::HelloServer);
                pkt.set_session_id(session_id);
                if (!pkt.SerializeToArray((void *) buf, (int) sizeof(buf))) {
                    ERROR_LOG(COMMON, "packet serialize error");
                }

                for (int i = 0; i < 10; ++i) {
                    udp_client.Send((const char *) buf, pkt.GetCachedSize());
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    u8 buf2[1024];
                    proto::Packet pkt2;
                    pkt2.Clear();
                    int n = udp_client.Recv((char *) buf2, sizeof(buf2));
                    if (0 < n) {
                        if (pkt2.ParseFromArray(buf2, n)) {
                            if (pkt2.hello_server_data().ok()) {
                                NOTICE_LOG(COMMON, "UDP session_id validation OK");
                                udp_session_ok = true;
                                user_id = pkt2.hello_server_data().user_id();
                                message_buf.SessionId(session_id);
                                NOTICE_LOG(COMMON, "user_id:%s", user_id.c_str());
                                break;
                            } else {
                                WARN_LOG(COMMON, "UDP session_id validation NG");
                            }
                        } else {
                            ERROR_LOG(COMMON, "packet deserialize error");
                        }
                    }
                }
            }

            if (udp_session_ok) {
                // discard first message
                send_buf_mtx.lock();
                for (int i = 0; i < kFirstMessageSize; ++i) {
                    send_buf.pop_front();
                }
                send_buf_mtx.unlock();
            } else {
                ERROR_LOG(COMMON, "UDP session failed");
                CloseUdpClientWithReason("cl_session_failed");
            }
        }

        send_buf_mtx.lock();
        int n = send_buf.size();
        if (n == 0) {
            send_buf_mtx.unlock();
        } else {
            n = std::min<int>(n, sizeof(buf));
            for (int i = 0; i < n; ++i) {
                buf[i] = send_buf.front();
                send_buf.pop_front();
            }
            send_buf_mtx.unlock();

            if (tcp_client.IsConnected()) {
                int m = tcp_client.Send((char *) buf, n);
                if (m < n) {
                    send_buf_mtx.lock();
                    for (int i = n - 1; m <= i; --i) {
                        send_buf.push_front(buf[i]);
                    }
                    send_buf_mtx.unlock();
                }
            } else if (udp_client.IsConnected()) {
                if (message_buf.CanPush()) {
                    message_buf.PushBattleMessage(user_id, buf, n);
                    if (message_buf.Packet().SerializeToArray((void *) buf, (int) sizeof(buf))) {
                        if (udp_client.Send((const char *) buf, message_buf.Packet().GetCachedSize())) {
                            udp_retransmit_countdown = 16;
                        } else {
                            udp_retransmit_countdown = 4;
                        }
                    } else {
                        ERROR_LOG(COMMON, "packet serialize error");
                    }
                } else {
                    send_buf_mtx.lock();
                    for (int i = n - 1; 0 <= i; --i) {
                        send_buf.push_front(buf[i]);
                    }
                    send_buf_mtx.unlock();
                    WARN_LOG(COMMON, "message_buf is full");
                }
            }
            updated = true;
        }

        if (!updated && udp_client.IsConnected()) {
            if (udp_retransmit_countdown-- == 0) {
                if (message_buf.Packet().SerializeToArray((void *) buf, (int) sizeof(buf))) {
                    if (udp_client.Send((const char *) buf, message_buf.Packet().GetCachedSize())) {
                        udp_retransmit_countdown = 16;
                    } else {
                        udp_retransmit_countdown = 4;
                    }
                } else {
                    ERROR_LOG(COMMON, "packet serialize error");
                }
            }
        }

        if (tcp_client.IsConnected()) {
            n = tcp_client.ReadableSize();
            if (0 < n) {
                n = std::min<int>(n, sizeof(buf));
                n = tcp_client.Recv((char *) buf, n);
                if (0 < n) {
                    recv_buf_mtx.lock();
                    for (int i = 0; i < n; ++i) {
                        recv_buf.push_back(buf[i]);
                    }
                    recv_buf_mtx.unlock();
                    updated = true;
                }
            }
        } else if (udp_client.IsConnected()) {
            n = udp_client.ReadableSize();
            if (0 < n) {
                n = std::min<int>(n, sizeof(buf));
                n = udp_client.Recv((char *) buf, n);
                if (0 < n) {
                    pkt.Clear();
                    if (pkt.ParseFromArray(buf, n)) {
                        if (pkt.type() == proto::MessageType::Battle) {
                            message_buf.ApplySeqAck(pkt.seq(), pkt.ack());
                            recv_buf_mtx.lock();
                            const auto &msgs = pkt.battle_data();
                            for (auto &msg : pkt.battle_data()) {
                                if (message_filter.IsNextMessage(msg)) {
                                    for (auto c : msg.body()) {
                                        recv_buf.push_back(c);
                                    }
                                }
                            }
                            recv_buf_mtx.unlock();
                        } else if (pkt.type() == proto::MessageType::Fin) {
                            CloseUdpClientWithReason("cl_recv_fin");
                        } else {
                            WARN_LOG(COMMON, "recv unexpected pkt type %d", pkt.type());
                        }
                    } else {
                        ERROR_LOG(COMMON, "packet deserialize error");
                    }
                    updated = true;
                }
            }
        }
    }
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
}

void Gdxsv::CloseUdpClientWithReason(const char *reason) {
    if (udp_client.IsConnected()) {
        proto::Packet pkt;
        pkt.Clear();
        pkt.set_type(proto::MessageType::Fin);
        pkt.set_session_id(session_id);
        pkt.mutable_fin_data()->set_detail(reason);

        char buf[1024];
        if (pkt.SerializePartialToArray((void *) buf, (int) sizeof(buf))) {
            udp_client.Send((const char *) buf, pkt.GetCachedSize());
        } else {
            ERROR_LOG(COMMON, "packet serialize error");
        }

        udp_client.Close();
    }
}

void Gdxsv::handleReleaseJSON(const std::string &json) {
    std::regex rgx("\"tag_name\":\"v.*?(?=\")");
    std::smatch match;

    if (std::regex_search(json.begin(), json.end(), match, rgx)) {
        latest_version = match.str(0).substr(13, std::string::npos);

        std::string current_version = std::string(REICAST_VERSION);
        current_version = current_version.substr(1, current_version.find_first_of("+") - 1);

        auto version_compare = [](std::string v1, std::string v2) {
            size_t i = 0, j = 0;
            while (i < v1.length() || j < v2.length()) {
                int acc1 = 0, acc2 = 0;

                while (i < v1.length() && v1[i] != '.') {
                    acc1 = acc1 * 10 + (v1[i] - '0');
                    i++;
                }
                while (j < v2.length() && v2[j] != '.') {
                    acc2 = acc2 * 10 + (v2[j] - '0');
                    j++;
                }

                if (acc1 < acc2) return false;
                if (acc1 > acc2) return true;

                ++i;
                ++j;
            }
            return false;
        };

        if (version_compare(latest_version, current_version)) {
            update_available = true;
        }
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
    os_LaunchFromURL("http://github.com/inada-s/flycast/releases/latest/");
    update_available = false;
}

void Gdxsv::DismissUpdateDialog() {
    update_available = false;
}

std::string Gdxsv::LatestVersion() {
    return latest_version;
}

void Gdxsv::StartReplay() {
    int me = 0;
    maxlag = 16;
    static proto::BattleLogFile btlLog;
    static std::deque<u8> replay_data;

    auto read_sendbuf = [this](McsMessage &msg) -> int {
        int n = msg.Deserialize(send_buf);
        if (0 < n) {
            send_buf.erase(send_buf.begin(), send_buf.begin() + n);
        }
        return n;
    };

    auto read_replay = [this](const std::function<bool(const McsMessage &)> &onread) {
        McsMessage msg;
        for (;;) {
            int n = msg.Deserialize(replay_data);
            if (0 < n) {
                if (onread(msg)) {
                    replay_data.erase(replay_data.begin(), replay_data.begin() + n);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    };


    static int connection_status = 0;
    int new_connection_status = ReadMem8_nommu(0x0c3abb88);
    if (connection_status != new_connection_status) {
        NOTICE_LOG(COMMON, "CON_ST: %x -> %x", connection_status, new_connection_status);
        connection_status = new_connection_status;
    }

    if (replay_state) {
        // TODO: NEED Patch - Skip ALL self MsgPush function
        WriteMem16_nommu(0x8c045f64, 9);
        WriteMem16_nommu(0x8c045e70, 9);
    }

    if (replay_state == 1) {
        replay_state = 2;

        NOTICE_LOG(COMMON, "StartReplay");
        std::string logfile = "diskdc2-1612648435300.pb";
        // std::string logfile = "diskdc2-1612801102972.pb";

        FILE *fp = nowide::fopen(logfile.c_str(), "rb");
        if (fp == nullptr) {
            NOTICE_LOG(COMMON, "fopen failed");
        }

        bool ok = btlLog.ParseFromFileDescriptor(fp->_file);
        if (!ok) {
            NOTICE_LOG(COMMON, "ParseFromFileDescriptor failed");
        }

        NOTICE_LOG(COMMON, "game_disk = %s", btlLog.game_disk().c_str());
        if (fp != nullptr) {
            fclose(fp);
        }

        for (int i = 0; i < btlLog.battle_data_size(); ++i) {
            const auto &data = btlLog.battle_data(i);
            std::copy(std::begin(data.body()), std::end(data.body()), std::back_inserter(replay_data));
        }

        return;
    }

    if (replay_state == 2) {
        replay_state = 3;
        return;
    }

    if (replay_state == 3) {
        send_buf_mtx.lock();
        LbsMessage msg;
        int sz = msg.Deserialize(send_buf);
        send_buf.erase(send_buf.begin(), send_buf.begin() + sz);
        send_buf_mtx.unlock();

        if (sz != 0) {
            recv_buf_mtx.lock();
            NOTICE_LOG(COMMON, "RECV cmd=%04x", msg.command);

            if (msg.command == LbsMessage::lbsLobbyMatchingEntry) {
                LbsMessage::SvAnswer(msg).Serialize(recv_buf);
                LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskMatchingJoin) {
                int n = btlLog.users_size();
                LbsMessage::SvAnswer(msg).Write8(4)->Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskPlayerSide) {
                // camera player id
                LbsMessage::SvAnswer(msg).Write8(me + 1)->Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskPlayerInfo) {
                int pos = msg.Read8();
                NOTICE_LOG(COMMON, "pos=%d", pos);
                const auto &user = btlLog.users(pos - 1);
                LbsMessage::SvAnswer(msg).
                        Write8(pos)->
                        WriteString(user.user_id())->
                        WriteString(user.user_name())-> // TODO: need UTF8 -> SJIS
                        WriteString(user.game_param())->
                        Write16(0)-> // grade
                        Write16(user.win_count())->
                        Write16(user.lose_count())->
                        Write16(0)->
                        Write16(user.battle_count() - user.win_count() - user.lose_count())->
                        Write16(0)->
                        Write16(1 + (pos - 1) / 2)-> // TODO TEAM
                        Write16(0)->
                        Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskRuleData) {
                LbsMessage::SvAnswer(msg).
                        WriteBytes(btlLog.rule_bin())->
                        Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskBattleCode) {
                LbsMessage::SvAnswer(msg).
                        WriteString(btlLog.battle_code())->
                        Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskMcsVersion) {
                LbsMessage::SvAnswer(msg).
                        Write8(10)->Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsAskMcsAddress) {
                LbsMessage::SvAnswer(msg).
                        Write16(4)->Write8(127)->Write8(0)->Write8(0)->Write8(1)->
                        Write16(2)->Write16(3333)->Serialize(recv_buf);
            }

            if (msg.command == LbsMessage::lbsLogout) {
                replay_state++;
            }

            recv_buf_mtx.unlock();
            return;
        }
    }

    if (replay_state == 4) {
        // wait until connect
        return;
    }

    if (replay_state == 5) {
        // get session_id from client
        recv_buf_mtx.lock();
        recv_buf.assign({0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd});
        recv_buf_mtx.unlock();
        replay_state++;
        return;
    }

    if (replay_state == 6) {
        send_buf_mtx.lock();
        if (20 <= send_buf.size()) {
            send_buf.clear();
            replay_state++;
        }
        send_buf_mtx.unlock();
        replay_seq = 0;
        replay_ts = btlLog.battle_data(replay_seq).timestamp();
        return;
    }

    if (replay_state == 7) {
        send_buf_mtx.lock();
        recv_buf_mtx.lock();

        McsMessage tx, rx;
        if (read_sendbuf(tx)) {
            NOTICE_LOG(COMMON, "STATE %d", (int) replay_state);
            NOTICE_LOG(COMMON, ">>> %dp %s\t%s", tx.sender, McsMsgKindName[int(tx.kind)], tx.to_hex().c_str());
            if (tx.kind == McsMsgKind::PingMsg) {
                replay_state++;
            }
        }

        read_replay([this, me](const McsMessage &rx) {
            if (rx.sender != me &&
                (rx.kind == McsMsgKind::IntroMsg || rx.kind == McsMsgKind::IntroMsgReturn)) {
                std::copy(std::begin(rx.body), std::end(rx.body), std::back_inserter(recv_buf));
                NOTICE_LOG(COMMON, "<<< %dp %s\t%s", rx.sender, McsMsgKindName[int(rx.kind)], rx.to_hex().c_str());
            }
            if (rx.kind == McsMsgKind::PongMsg) return false;
            return true;
        });

        send_buf_mtx.unlock();
        recv_buf_mtx.unlock();
    }

    if (replay_state == 8) {
        send_buf_mtx.lock();
        recv_buf_mtx.lock();

        McsMessage tx;
        if (read_sendbuf(tx)) {
            NOTICE_LOG(COMMON, "STATE %d", (int) replay_state);
            NOTICE_LOG(COMMON, "send_buf %dp %s\t%s", tx.sender, McsMsgKindName[int(tx.kind)], tx.to_hex().c_str());

            if (tx.kind == McsMsgKind::StartMsg) {
                replay_state++;
            }
        }

        read_replay([this, me](const McsMessage &rx) {
            if (rx.sender != me && rx.kind == McsMsgKind::PongMsg) {
                std::copy(std::begin(rx.body), std::end(rx.body), std::back_inserter(recv_buf));
                NOTICE_LOG(COMMON, "<<< %dp %s\t%s", rx.sender, McsMsgKindName[int(rx.kind)], rx.to_hex().c_str());
            }
            if (rx.kind == McsMsgKind::StartMsg) return false;
            return true;
        });

        send_buf_mtx.unlock();
        recv_buf_mtx.unlock();
    }

    if (replay_state == 9) {
        send_buf_mtx.lock();
        recv_buf_mtx.lock();

        if (connection_status == 5) {
            replay_state++;
        }

        read_replay([this, me](const McsMessage &rx) {
            if (rx.sender != me && rx.kind == McsMsgKind::StartMsg) {
                std::copy(std::begin(rx.body), std::end(rx.body), std::back_inserter(recv_buf));
                NOTICE_LOG(COMMON, "<<< %dp %s\t%s", rx.sender, McsMsgKindName[int(rx.kind)], rx.to_hex().c_str());
            }
            if (rx.kind == McsMsgKind::KeyMsg) return false;
            return true;
        });

        send_buf_mtx.unlock();
        recv_buf_mtx.unlock();
    }

    if (replay_state == 10) {
        send_buf_mtx.lock();
        recv_buf_mtx.lock();
        McsMessage tx;
        if (read_sendbuf(tx)) {
            NOTICE_LOG(COMMON, "STATE %d", (int) replay_state);
            NOTICE_LOG(COMMON, "send_buf %dp %s\t%s", tx.sender, McsMsgKindName[int(tx.kind)], tx.to_hex().c_str());

            if (tx.kind == McsMsgKind::KeyMsg) {
                read_replay([this, &tx, me](const McsMessage &rx) {
                    if (rx.kind == McsMsgKind::KeyMsg) {
                        if (rx.tail_frame() <= tx.tail_frame()) {
                            std::copy(std::begin(rx.body), std::end(rx.body), std::back_inserter(recv_buf));
                            NOTICE_LOG(COMMON, "<<< %dp %s\t%s", rx.sender, McsMsgKindName[int(rx.kind)],
                                       rx.to_hex().c_str());
                            return true;
                        } else {
                            return false;
                        }
                    }
                    if (rx.sender != me && (rx.kind == McsMsgKind::LoadStartMsg || rx.kind == McsMsgKind::LoadEndMsg)) {
                        std::copy(std::begin(rx.body), std::end(rx.body), std::back_inserter(recv_buf));
                        return true;
                    }
                    if (rx.kind == McsMsgKind::PingMsg) {
                        replay_state = 7;
                        return false;
                    }
                    return true;
                });
            }
        }
        send_buf_mtx.unlock();
        recv_buf_mtx.unlock();
    }
}

Gdxsv gdxsv;
