#pragma once

#include "types.h"

#include <vector>
#include <deque>

enum class McsMsgKind {
    ConnectionIdMsg,
    StartMsg,
    IntroMsg,
    IntroMsgReturn,
    KeyMsg,
    PingMsg,
    PongMsg,
    LoadStartMsg,
    LoadEndMsg,
    LagControlTestMsg,
    UnknownMsg,
};

static const char * const McsMsgKindName[] = {
        "ConnectionIdMsg",
        "StartMsg",
        "IntroMsg",
        "IntroMsgReturn",
        "KeyMsg",
        "PingMsg",
        "PongMsg",
        "LoadStartMsg",
        "LoadEndMsg",
        "LagControlTestMsg",
        "UnknownMsg",
};

class McsMessage {
public:
    template<typename T>
    int Deserialize(const T &buf) {
        if (buf.size() < 4) {
            return 0;
        }

        kind = McsMsgKind::UnknownMsg;

        if (buf[0] == 0x82 && buf[1] == 0x02) {
            kind = McsMsgKind::ConnectionIdMsg;
            int n = 20;
            sender = 0;
            body.clear();
            for (int i = 0; i < n; ++i) {
                body.push_back(buf[i]);
            }
            return n;
        }

        int n = buf[0];
        if (buf.size() < n) {
            return 0;
        }

        body.clear();
        for (int i = 0; i < n; ++i) {
            body.push_back(buf[i]);
        }

        int k = (buf[1] & 0xf0) >> 4;
        int p = buf[1] & 0x0f;
        int param1 = buf[2];
        int param2 = buf[3];
        sender = p;

        if (k == 1 && param1 == 0) kind = McsMsgKind::IntroMsg;
        if (k == 1 && param1 == 1) kind = McsMsgKind::IntroMsgReturn;
        if (k == 2) kind = McsMsgKind::KeyMsg;
        if (k == 3 && param1 == 0) kind = McsMsgKind::PingMsg;
        if (k == 3 && param1 == 1) kind = McsMsgKind::PongMsg;
        if (k == 4) kind = McsMsgKind::StartMsg;
        if (k == 5 && param1 == 0) kind = McsMsgKind::LoadStartMsg;
        if (k == 5 && param1 == 1) kind = McsMsgKind::LoadEndMsg;
        if (k == 9) kind = McsMsgKind::LagControlTestMsg;

        return n;
    }

    std::string to_hex() const {
        std::string ret(body.size() * 2, ' ');
        for (int i = 0; i < body.size(); i++) {
            std::sprintf(&ret[0] + i * 2, "%02x", body[i]);
        }
        return ret;
    }

    int tail_frame() const {
        if (kind == McsMsgKind::KeyMsg && 18 <= body.size()) {
            return int(body[17]) << 8 | int(body[16]);
        }
        return 0;
    }

    int sender;
    McsMsgKind kind;
    std::vector <u8> body;
};
