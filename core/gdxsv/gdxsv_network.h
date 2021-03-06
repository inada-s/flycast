#pragma once

#include <string>
#include <mutex>
#include <deque>

#include "types.h"
#include "network/net_platform.h"
#include "gdxsv.pb.h"

class TcpClient {
public:
    ~TcpClient() {
        Close();
    }

    bool Connect(const char *host, int port);

    void SetNonBlocking();

    int IsConnected() const;

    int Recv(char *buf, int len);

    int Send(const char *buf, int len);

    void Close();

    u32 ReadableSize() const;

    const std::string &host() { return host_; }

    const std::string &local_ip() const { return local_ip_; }

    int port() const { return port_; }

private:
    sock_t sock_ = INVALID_SOCKET;
    std::string host_;
    std::string local_ip_;
    int port_;
};

class MessageBuffer {
public:
    static const int kBufSize = 50;

    MessageBuffer();

    void SessionId(const std::string &session_id);

    bool CanPush() const;

    bool PushBattleMessage(const std::string &user_id, u8 *body, u32 body_length);

    const proto::Packet &Packet();

    void ApplySeqAck(u32 seq, u32 ack);

    void Clear();

private:
    u32 msg_seq_;
    u32 snd_seq_;
    proto::Packet packet_;
};

class MessageFilter {
public:
    bool IsNextMessage(const proto::BattleMessage &msg);

    void Clear();

private:
    std::map<std::string, u32> recv_seq;
};


class UdpRemote {
public:
    bool Open(const char *host, int port);

    bool Open(const std::string &addr);

    void Close();

    bool is_open() const;

    const std::string &str_addr() const;

    const sockaddr_in &net_addr() const;

private:
    bool is_open_;
    std::string str_addr_;
    sockaddr_in net_addr_;
};

class UdpClient {
public:
    bool Bind(int port);

    bool Initialized() const;

    int RecvFrom(char *buf, int len, std::string &sender);

    int SendTo(const char *buf, int len, const UdpRemote &remote);

    u32 ReadableSize() const;

    void Close();

    int bind_port() const { return bind_port_; }

private:
    sock_t sock_ = INVALID_SOCKET;
    int bind_port_;
    std::string bind_ip_;
};
