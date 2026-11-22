// Host test for patches/nbt-aad-audio-route-lifetime.patch and patches/nbt-aad-browser-volume-lock.patch.
#include "audio/browser_audio.hpp"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

static_assert(!std::is_copy_constructible<nbt::AudioRoute>::value, "AudioRoute must not be copyable");
static_assert(!std::is_copy_assignable<nbt::AudioRoute>::value, "AudioRoute must not be copy-assignable");

namespace {

pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
int g_dev_writes = 0;
int g_dev_opens = 0;
int g_fail_next_writes = 0;
int16_t g_last_peak = 0;
int16_t g_max_peak_seen = 0;
int g_speech_frames = 0;
int g_speech_states = 0;

void resetDeviceStats() {
    pthread_mutex_lock(&g_dev_lock);
    g_dev_writes = 0;
    g_dev_opens = 0;
    g_fail_next_writes = 0;
    g_last_peak = 0;
    g_max_peak_seen = 0;
    pthread_mutex_unlock(&g_dev_lock);
}

int deviceWrites() {
    pthread_mutex_lock(&g_dev_lock);
    int v = g_dev_writes;
    pthread_mutex_unlock(&g_dev_lock);
    return v;
}

int deviceOpens() {
    pthread_mutex_lock(&g_dev_lock);
    int v = g_dev_opens;
    pthread_mutex_unlock(&g_dev_lock);
    return v;
}

int16_t lastPeak() {
    pthread_mutex_lock(&g_dev_lock);
    int16_t v = g_last_peak;
    pthread_mutex_unlock(&g_dev_lock);
    return v;
}

int16_t maxPeakSeen() {
    pthread_mutex_lock(&g_dev_lock);
    int16_t v = g_max_peak_seen;
    pthread_mutex_unlock(&g_dev_lock);
    return v;
}

nbt::BrowserAudio* g_active_probe_target = NULL;
volatile int g_active_probe_calls = 0;

long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

// Fake AsoundDevice matching src/android_auto/audio_a2i/asound_device.hpp.
namespace nbt {

AsoundDevice::AsoundDevice()
    : want_rate_(48000), force_rate_(0), lib_(NULL), pcm_(NULL), rate_(48000), frag_bytes_(4096),
      cur_(NULL), cur_size_(0), cur_fill_(0), handed_(0), write_errors_(0), prepares_(0),
      writes_(0), writes_blocked_(0), lead_min_(0), lead_max_(0), lead_seen_(false) {
    memset(fn_, 0, sizeof(fn_));
}

AsoundDevice::~AsoundDevice() { close(); }

void AsoundDevice::configure(const char*, uint32_t want_rate, uint32_t force_rate, const char*, int, int) {
    want_rate_ = want_rate ? want_rate : 48000;
    force_rate_ = force_rate;
}

bool AsoundDevice::load() { return true; }
void AsoundDevice::unload() { close(); }

bool AsoundDevice::open() {
    if (pcm_) return true;
    rate_ = force_rate_ ? force_rate_ : want_rate_;
    frag_bytes_ = 4096; // 1024 stereo frames (~21.3 ms at 48 kHz)
    pcm_ = reinterpret_cast<void*>(1);
    pthread_mutex_lock(&g_dev_lock);
    ++g_dev_opens;
    pthread_mutex_unlock(&g_dev_lock);
    return true;
}

void AsoundDevice::close() { pcm_ = NULL; }

bool AsoundDevice::write(const int16_t* buf, size_t frames) {
    pthread_mutex_lock(&g_dev_lock);
    if (g_fail_next_writes > 0) {
        --g_fail_next_writes;
        ++write_errors_;
        pthread_mutex_unlock(&g_dev_lock);
        return false;
    }
    int16_t peak = 0;
    for (size_t i = 0; i < frames * kChannels; ++i) {
        int16_t v = buf[i] < 0 ? (int16_t)-buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    g_last_peak = peak;
    if (peak > g_max_peak_seen) g_max_peak_seen = peak;
    ++g_dev_writes;
    pthread_mutex_unlock(&g_dev_lock);
    usleep(5000);
    return true;
}

// Stubs for SpeechOut (matches aa_audio.cpp) and MicUplink.
bool MicUplink::start() { return false; }
void A2ICaptureStream::stop() {}

SpeechOut::SpeechOut(AudioRoute& route) : route_(route), focus_(*this) {}
SpeechOut::~SpeechOut() {}
void SpeechOut::init(const SpeechConfig&) {}
bool SpeechOut::takesPrompt(uint64_t) const { return false; }
void SpeechOut::frame(const uint8_t*, size_t) { ++g_speech_frames; }
void SpeechOut::state(bool, uint32_t, uint32_t) { ++g_speech_states; }
bool SpeechOut::active() const {
    if (g_active_probe_target) {
        // Must not deadlock: BrowserAudio::lock_ must NOT be held when SpeechOut::active() is called.
        g_active_probe_target->setMediaGain(0.5f);
        ++g_active_probe_calls;
    }
    return false;
}
void SpeechOut::onSessionEnded() {}
bool SpeechOut::stop() { return false; }
bool SpeechOut::queueBuild() { return false; }
bool SpeechOut::queueRemove() { return false; }
void SpeechOut::cancelPending() {}
void SpeechOut::setFeed(bool) {}
void SpeechOut::flush() {}
size_t SpeechOut::queuedBytes() const { return 0; }
void SpeechOut::setFocusState(bool, bool, uint64_t) {}
void SpeechOut::openEndpoint() {}
void SpeechOut::closeEndpoint() {}
uint32_t SpeechOut::endpointWrites() const { return 0; }

} // namespace nbt

static void feedMedia(nbt::BrowserAudio& audio, int16_t sample, int blocks) {
    // 1024 stereo frames = 4096 bytes per block at 48 kHz
    std::vector<int16_t> pcm(1024 * 2, sample);
    for (int i = 0; i < blocks; ++i) {
        audio.onFrame(nbt::AA_CH_MEDIA_AUDIO, reinterpret_cast<const uint8_t*>(&pcm[0]),
                      pcm.size() * sizeof(int16_t), 0);
    }
}

static void testInitialVolumeAndReopenAndNoNestedLock() {
    resetDeviceStats();
    nbt::BrowserConfig cfg;
    cfg.conn = 0;
    cfg.preroll_ms = 0;
    cfg.wait_ms = 0;
    cfg.resampler_linear = true;
    cfg.drift = false;
    cfg.speech.enabled = false;

    nbt::BrowserAudio audio;
    assert(audio.init(cfg));

    // Set volume to 0 BEFORE the mixer thread / openPcm() starts (exact aa_audio::setup() sequence).
    audio.setMediaGain(0.0f);
    audio.onStreamState(nbt::AA_CH_MEDIA_AUDIO, true, 48000, 2);
    feedMedia(audio, 10000, 12);

    const long long deadline1 = nowMs() + 3000;
    while (deviceWrites() < 6 && nowMs() < deadline1) usleep(5000);
    assert(deviceWrites() >= 6);
    // openPcm() must preserve the 0.0f gain instead of resetting it to 1.0f.
    assert(maxPeakSeen() == 0);

    // Enable the SpeechOut::active() re-entrancy probe: sets gain to 0.5f from inside SpeechOut::active().
    // Would deadlock immediately if BrowserAudio::lock_ were held across SpeechOut::active().
    g_active_probe_calls = 0;
    g_active_probe_target = &audio;
    feedMedia(audio, 10000, 20);
    const long long deadline2 = nowMs() + 3000;
    while ((g_active_probe_calls < 3 || lastPeak() < 4500) && nowMs() < deadline2) {
        feedMedia(audio, 10000, 2);
        usleep(5000);
    }
    g_active_probe_target = NULL;
    assert(g_active_probe_calls >= 3);
    assert(lastPeak() >= 4500 && lastPeak() <= 5500);

    // Set gain to 0.25f and trigger 50 consecutive write failures so mixerLoop() reopens the device via openPcm().
    audio.setMediaGain(0.25f);
    pthread_mutex_lock(&g_dev_lock);
    g_fail_next_writes = 50;
    pthread_mutex_unlock(&g_dev_lock);
    const long long deadline3 = nowMs() + 4000;
    while (deviceOpens() < 2 && nowMs() < deadline3) {
        feedMedia(audio, 10000, 2);
        usleep(10000);
    }
    assert(deviceOpens() >= 2);
    const int writes_after_reopen = deviceWrites();
    const long long deadline4 = nowMs() + 3000;
    while (deviceWrites() < writes_after_reopen + 5 && nowMs() < deadline4) {
        feedMedia(audio, 10000, 2);
        usleep(5000);
    }
    assert(deviceWrites() >= writes_after_reopen + 5);
    // Reopen must preserve 0.25f gain (~2500), not clobber back to 1.0f (10000).
    assert(lastPeak() >= 2000 && lastPeak() <= 3000);

    audio.stop();
}

static void testDestructorDrainsShutdownRemove(const char* audioconn_script, const char* remove_done_file) {
    resetDeviceStats();
    setenv("NBT_AUDIOCONN", audioconn_script, 1);
    setenv("TEST_REMOVE_DONE", remove_done_file, 1);

    nbt::BrowserConfig cfg;
    cfg.conn = 0xb8;
    cfg.preroll_ms = 0;
    cfg.wait_ms = 0;
    cfg.resampler_linear = true;
    cfg.drift = false;
    cfg.speech.enabled = false;

    nbt::BrowserAudio* audio = new nbt::BrowserAudio();
    assert(audio->init(cfg));
    audio->onStreamState(nbt::AA_CH_MEDIA_AUDIO, true, 48000, 2);
    feedMedia(*audio, 10000, 4);

    // Wait for audioconn -b 0xb8 to be granted and audio to start writing.
    const long long deadline = nowMs() + 3000;
    while (deviceWrites() < 2 && nowMs() < deadline) usleep(5000);
    assert(deviceWrites() >= 2);
    // Ensure the build worker has finished so route_policy_ is ROUTED.
    audio->waitForRoute();

    // Destroy directly WITHOUT calling waitForRoute() after stop():
    // ~BrowserAudio() must drain the shutdown audioconn -r 0xb8 before destroying lock_/route_policy_
    // and without ~AudioRoute() cancelling it.
    const long long t0 = nowMs();
    delete audio;
    const long long elapsed = nowMs() - t0;
    assert(elapsed >= 150);
    assert(access(remove_done_file, F_OK) == 0);
}

static void testGuidanceMixedWithMedia(const char* audioconn_script) {
    resetDeviceStats();
    g_speech_frames = g_speech_states = 0;
    setenv("NBT_AUDIOCONN", audioconn_script, 1);
    setenv("NBT_SPEECH_OWN_CHANNEL", "1", 1);

    nbt::BrowserConfig cfg;
    cfg.conn = 0xb8;
    cfg.preroll_ms = 0;
    cfg.wait_ms = 0;
    cfg.resampler_linear = true;
    cfg.drift = false;
    cfg.speech.enabled = false;

    nbt::BrowserAudio audio;
    assert(audio.init(cfg));
    audio.onStreamState(nbt::AA_CH_MEDIA_AUDIO, true, 48000, 2);
    feedMedia(audio, 0, 8);
    const long long media_deadline = nowMs() + 3000;
    while (deviceWrites() < 2 && nowMs() < media_deadline) usleep(5000);
    assert(deviceWrites() >= 2);
    audio.waitForRoute();

    audio.onStreamState(nbt::AA_CH_GUIDANCE_AUDIO, true, 48000, 1);
    std::vector<int16_t> guidance(1024, 10000);
    const long long guidance_deadline = nowMs() + 3000;
    while (maxPeakSeen() < 4000 && nowMs() < guidance_deadline) {
        feedMedia(audio, 0, 2);
        audio.onFrame(nbt::AA_CH_GUIDANCE_AUDIO, reinterpret_cast<const uint8_t*>(&guidance[0]),
                      guidance.size() * sizeof(int16_t), 0);
        usleep(5000);
    }
    assert(maxPeakSeen() >= 4000);
    audio.onStreamState(nbt::AA_CH_GUIDANCE_AUDIO, false, 0, 0);
    assert(g_speech_frames == 0 && g_speech_states == 0);
    audio.stop();
    unsetenv("NBT_SPEECH_OWN_CHANNEL");
}

int main(int argc, char** argv) {
    assert(argc == 3);
    testInitialVolumeAndReopenAndNoNestedLock();
    testDestructorDrainsShutdownRemove(argv[1], argv[2]);
    testGuidanceMixedWithMedia(argv[1]);
    return 0;
}
