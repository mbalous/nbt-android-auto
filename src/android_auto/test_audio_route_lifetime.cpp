#include "audio/audio_route.hpp"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void done(void* p) { *static_cast<bool*>(p) = true; }

static long long milliseconds() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    setenv("NBT_AUDIOCONN", argv[1], 1);
    setenv("TEST_ROUTE_STARTED", argv[2], 1);
    bool completed = false;
    nbt::AudioRoute route;
    assert(route.configure());
    assert(route.build(0xb8, done, &completed));
    for (int i = 0; i < 200 && access(argv[2], F_OK); ++i) usleep(10000);
    assert(access(argv[2], F_OK) == 0);
    route.cancelPending();
    route.waitIdle();
    assert(completed);
    completed = false;
    assert(route.remove(0xb8, done, &completed));
    long long start = milliseconds();
    route.waitIdle();
    assert(completed);
    assert(milliseconds() - start >= 300);
    return 0;
}
