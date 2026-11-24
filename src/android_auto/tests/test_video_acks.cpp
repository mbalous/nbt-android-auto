#include "../video_acks.hpp"
#include <assert.h>
#include <stdio.h>

static uint64_t frame(nbt::VideoAcks& acks, double now)
{
    bool overflow;
    const uint64_t token = acks.frame(now, overflow);
    assert(!overflow);
    return token;
}

int main()
{
    nbt::VideoAcks acks;
    bool forced;
    acks.start(true, true, 20);
    const uint64_t first = frame(acks, 1.0);
    const uint64_t second = frame(acks, 1.0);
    assert(first && second != first);
    assert(acks.consumed(second));
    assert(!acks.take(1.0, forced));
    assert(acks.consumed(first));
    assert(acks.take(1.0, forced) && !forced);
    assert(!acks.take(1.01, forced));
    assert(acks.tickMs(1.01) <= 41);
    assert(acks.take(1.051, forced) && !forced);

    const uint64_t stalled = frame(acks, 2.0);
    assert(!acks.take(16.99, forced));
    assert(acks.take(17.0, forced) && forced);
    const uint64_t later = frame(acks, 17.0);
    assert(!acks.consumed(stalled));
    assert(!acks.take(17.1, forced));
    assert(acks.consumed(later) && acks.take(17.1, forced) && !forced);

    const uint64_t old = frame(acks, 18.0);
    assert(acks.resetStream() == 1);
    const uint64_t fresh = frame(acks, 18.0);
    assert(!acks.consumed(old) && !acks.take(18.0, forced));
    assert(acks.consumed(fresh) && acks.take(18.0, forced));

    acks.start(true, false, 20);
    assert(frame(acks, 20.0) && acks.take(20.0, forced));
    assert(frame(acks, 20.0) && !acks.take(20.01, forced));
    assert(acks.take(20.051, forced));
    acks.start(false, false, 0);
    assert(!frame(acks, 21.0) && !acks.take(21.0, forced));

    acks.start(true, true, 20);
    uint64_t overflowed = frame(acks, 22.0);
    for (unsigned i = 1; i < 64; ++i) assert(frame(acks, 22.0));
    bool overflow = false;
    assert(acks.frame(22.0, overflow) && overflow);
    assert(!acks.consumed(overflowed) && acks.resetStream() == 64);
    puts("VIDEO_ACKS_OK");
}
