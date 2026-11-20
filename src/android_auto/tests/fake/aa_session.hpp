#ifndef TEST_AA_SESSION_HPP
#define TEST_AA_SESSION_HPP

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

namespace nbt {

// Host capture tests feed the media callbacks directly; the session must not run.
class AASession {
public:
    void setVideoFrameCallback(void (*)(uint32_t, const uint8_t*, size_t, uint64_t, void*), void*) { abort(); }
    void setCodecConfigCallback(void (*)(const uint8_t*, size_t, void*), void*) { abort(); }
    void setNavStatusCallback(void (*)(uint16_t, const uint8_t*, size_t, void*), void*) { abort(); }
    void setVideoFocusCallback(void (*)(bool, uint32_t, void*), void*) { abort(); }
    void setAudioFrameCallback(void (*)(uint8_t, const uint8_t*, size_t, uint64_t, void*), void*) { abort(); }
    void setAudioStateCallback(void (*)(uint8_t, bool, uint32_t, uint32_t, void*), void*) { abort(); }
    void setDeferredAudioAcks(bool) { abort(); }
    bool start(int) { abort(); }
    void stop() { abort(); }
    bool isActive() const { abort(); }
    void postByeBye() { abort(); }
    void postVideoFocus(bool) { abort(); }
    void postAudioConsumed(uint8_t, size_t) { abort(); }
    void postNightMode(bool) { abort(); }
    void postRelativeEvent(unsigned, int) { abort(); }
    void postKeyEvent(unsigned, bool) { abort(); }
};

} // namespace nbt

#endif
