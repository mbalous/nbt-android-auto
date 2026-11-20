// Test the real f6 mixer without an A2I device.
#include "../aa_media_out.cpp"

using namespace nbt::mediaout;

static void check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}

static int sample(int percent) {
    setVolume(percent);
    g_s[S_MEDIA].put(10000, -10000);
    int16_t out[2] = {0, 0};
    mixMain(out, 1);
    check(out[0] == -out[1], "stereo gain differs");
    return out[0];
}

int main() {
    unsetenv("NBT_AA_VOLUME");
    for (int i = 0; i < S_N; ++i) g_s[i].init("test", 48000, 2, 1, 100, 10);
    check(sample(100) == 10000, "100% must not amplify");
    const int half = sample(50);
    check(half > 900 && half < 1100, "50% must use the shared dB curve");
    check(sample(0) == 0 && sample(-1) == 0, "zero and negative volume must be silent");
    check(sample(101) == 10000, "volume above 100 must be clamped");
    setenv("NBT_AA_VOLUME", "0", 1);
    check(sample(0) == 10000, "disabled software volume must preserve gain");
    for (int i = 0; i < S_N; ++i) free(g_s[i].buf);
    return 0;
}
