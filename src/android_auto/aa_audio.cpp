// aa_audio: optional Android Auto sound over the Android Auto connection. See aa_audio.hpp.
//
// Wiring, as in NBT-AAD's main.cpp (browser mode), for one session per process:
//   AASession audio frame / stream state (session thread) --> BrowserAudio::onFrame / onStreamState
//   BrowserAudio consumed bytes / underflow (mixer thread)  --> AASession::postAudioConsumed / postAudioUnderflow
// With deferred acks (NBT_AA_DEFER_ACKS, default on) the phone is paced by the car's audio clock: a frame is acked once
// the mixer has taken it, or after NBT_AA_ACK_FORCE_MS at the latest, so no device / no route never stalls the session.
#include "aa_audio.hpp"
#include "aa_session.hpp"
#include "audio/audio_acks.hpp"
#include "audio/browser_audio.hpp"
#include "audio/config.hpp"
#include "audio/mic_uplink.hpp"
#include "volume.hpp"
#include <stdio.h>
#include <stdlib.h>

namespace {

nbt::BrowserAudio* g_audio = 0;

void on_frame(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t pts, void*)
{
    if (g_audio)
        g_audio->onFrame(channel, pcm, len, pts);
}

void on_state(uint8_t channel, bool started, uint32_t rate, uint32_t channels, void*)
{
    if (g_audio)
        g_audio->onStreamState(channel, started, rate, channels);
}

void on_consumed(uint8_t channel, size_t bytes, void* session)
{
    static_cast<nbt::AASession*>(session)->postAudioConsumed(channel, bytes);
}

void on_underflow(uint8_t channel, void* session)
{
    static_cast<nbt::AASession*>(session)->postAudioUnderflow(channel);
}

} // namespace

namespace aa_audio {

bool setup(nbt::AASession& session)
{
    if (!nbt::browserAudioEnabled()) {
        printf("AA_AUDIO off (Bluetooth; NBT_AUDIO_MODE=%s)\n", nbt::audioModeName(nbt::audioMode()));
        return false;
    }
    nbt::BrowserConfig cfg = nbt::readBrowserConfig();
    cfg.speech.enabled = false; // original HU_NBT keeps guidance in the browser mix (no Evo /input_mono_0 nav prompter)
    g_audio = new nbt::BrowserAudio();
    if (!g_audio->init(cfg)) {
        printf("AA_AUDIO WARNING: A2I output (liba2i_client) not loaded: Android Auto audio will be silent\n");
        // native_service reads this before the next session and falls back to Bluetooth audio
        FILE* f = fopen("/fs/ram3/nbt-aa-native/audio-output.failed", "w");
        if (f) { fprintf(f, "A2I output not loaded\n"); fclose(f); }
        else perror("AA_AUDIO cannot record output failure");
    }
    if (cfg.defer_acks) {
        g_audio->setConsumedCallback(on_consumed, &session);
        g_audio->setAckWindow(nbt::AudioAcks::window());
    }
    session.setDeferredAudioAcks(cfg.defer_acks);
    if (cfg.underflow_reports)
        g_audio->setUnderflowCallback(on_underflow, &session);
    session.setUnderflowReports(cfg.underflow_reports);
    session.setAudioFrameCallback(on_frame, 0);
    session.setAudioStateCallback(on_state, 0);
    g_audio->start();
    g_audio->onSessionStarted();
    {
        // start at the car's last known volume (nbt_knob keeps it); without it the gain stays at 0 dB until the next step
        FILE* v = fopen("/fs/ram3/nbt-aa-native/volume", "r");
        int pct;
        if (v) {
            if (fscanf(v, "%d", &pct) == 1)
                setVolume(pct);
            fclose(v);
        }
    }
    printf("AA_AUDIO browser mode: connection 0x%x%s, acks %s, underflow reports %s\n", (unsigned)cfg.conn,
           cfg.conn ? "" : " (not routed: plays only if the car already listens to input_stereo_3)",
           cfg.defer_acks ? "once played" : "on arrival", cfg.underflow_reports ? "on" : "off");
    return true;
}

void setVolume(int percent)
{
    const char* off = getenv("NBT_AA_VOLUME");
    if (!g_audio || (off && off[0] == '0'))
        return;
    percent = nbt::volumeClampPct(percent);
    const int db = nbt::volumePctToDb(percent);
    // 0 % is silence (the car at volume 0), not just -40 dB
    const float gain = percent ? nbt::volumeDbToGain(db) : 0.0f;
    g_audio->setMediaGain(gain);
    printf("AA_AUDIO volume %d%% -> %d dB\n", percent, db);
}

void shutdown()
{
    if (!g_audio)
        return;
    g_audio->cancelPending();
    g_audio->waitForRoute();
    g_audio->onSessionEnded();
    g_audio->stop();
    g_audio->waitForRoute();
    delete g_audio;
    g_audio = 0;
    printf("AA_AUDIO stopped\n");
}

} // namespace aa_audio

// ---- link stubs ------------------------------------------------------------------------------------------------------
// AudioBackend's default startMic()/stopMic() (upstream audio/backend.hpp) reference MicUplink, which links the car's
// A2I client library directly. nbt_android_auto never starts the car microphone, so these two definitions replace
// upstream's mic_uplink.cpp / a2i_stream.cpp instead of adding liba2i_client.so as a load-time dependency.
bool nbt::MicUplink::start()
{
    printf("AA_AUDIO car microphone not supported\n");
    return false;
}

void nbt::A2ICaptureStream::stop() {}

// BrowserAudio owns a SpeechOut member for NBT Evo's separate nav prompter (/input_mono_0, which uses A2IPlaybackStream
// and links liba2i_client directly). Original HU_NBT keeps guidance in the browser mix, so stub SpeechOut here too.
nbt::SpeechOut::SpeechOut(AudioRoute& route) : route_(route), focus_(*this) {}
nbt::SpeechOut::~SpeechOut() {}
void nbt::SpeechOut::init(const SpeechConfig&) {}
bool nbt::SpeechOut::takesPrompt(uint64_t) const { return false; }
void nbt::SpeechOut::frame(const uint8_t*, size_t) {}
void nbt::SpeechOut::state(bool, uint32_t, uint32_t) {}
bool nbt::SpeechOut::active() const { return false; }
void nbt::SpeechOut::onSessionEnded() {}
bool nbt::SpeechOut::stop() { return false; }
bool nbt::SpeechOut::queueBuild() { return false; }
bool nbt::SpeechOut::queueRemove() { return false; }
void nbt::SpeechOut::cancelPending() {}
void nbt::SpeechOut::setFeed(bool) {}
void nbt::SpeechOut::flush() {}
size_t nbt::SpeechOut::queuedBytes() const { return 0; }
void nbt::SpeechOut::setFocusBusy(bool, uint64_t) {}
void nbt::SpeechOut::openEndpoint() {}
void nbt::SpeechOut::closeEndpoint() {}
uint32_t nbt::SpeechOut::endpointWrites() const { return 0; }
