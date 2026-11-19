#ifndef TEST_MOST_CLIENT_HPP
#define TEST_MOST_CLIENT_HPP
#include "most/msg.hpp"
#include "most/ids.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nbt {
namespace most {
enum LinkState { kUp };
class Client {
public:
    typedef void (*Tap)(const View&, MsgClass, void*);
    typedef void (*InfoTap)(uint32_t, const uint8_t*, uint32_t, void*);
    Client() : tap_(0), info_(0) {}
    bool setTap(Tap tap, void*) { tap_ = tap; return true; }
    bool setInfoTap(InfoTap info, void*) { info_ = info; return true; }
    bool start(const char*) {
        if (getenv("TEST_START_FAIL")) return false;
        const uint8_t registry[] = {1, 0, 0, 0, 0x0e, 0x10, 0, 3};
        info_(kAttrRegistry, registry, sizeof(registry), 0);
        return true;
    }
    LinkState state() const { return kUp; }
    unsigned deviceId() const { return 0x123; }
    bool send(const Msg& msg) {
        if (msg.fkt == 0x053) {
            FILE* f = fopen(NBT_AUDIO_RUNTIME_DIR "/audio-route.built", "r");
            if (!f) return false;
            fclose(f);
        }
        const char* mode = getenv("TEST_REPLY");
        if (mode && !strcmp(mode, "send-fail")) return false;
        if (mode && !strcmp(mode, "timeout")) return true;
        View reply = View();
        reply.fblock = msg.fblock;
        reply.fkt = msg.fkt;
        reply.dst = 0x123;
        reply.op = mode && !strcmp(mode, "error") ? 0x9 : 0xd;
        reply.data = &msg.data[0];
        reply.len = 2;
        tap_(reply, kNormal, 0);
        return true;
    }
private:
    Tap tap_;
    InfoTap info_;
};
}
}
#endif
