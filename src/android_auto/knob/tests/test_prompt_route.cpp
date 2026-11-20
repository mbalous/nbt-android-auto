#define NBT_AA_STATE_FILE "state"
#define NBT_AA_PROMPT_REQ "prompt"
#define NBT_AA_PROMPT_READY "ready"
#define main nbt_knob_main
#include "../nbt_knob.cpp"
#undef main

#include <assert.h>

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void wait_for_child() {
    for (int i = 0; i < 500 && g_promptChild; ++i) {
        g_promptNext = 0;
        promptRouteIfNeeded();
        usleep(10000);
    }
    assert(!g_promptChild);
}

int main(int argc, char** argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    write_file("audioconn",
        "#!/bin/sh\n"
        "if test \"$1\" = -b; then\n"
        "  echo \"$2\" > audio-route.built\n"
        "  if test -f slow-build; then sleep 0.1; fi\n"
        "  if test -f fail-build; then touch audio-route.failed; exit 1; fi\n"
        "else\n"
        "  if test -f fail-remove; then exit 1; fi\n"
        "  rm -f audio-route.built\n"
        "fi\n");
    assert(chmod("audioconn", 0700) == 0);
    write_file("state", "state=running\n");
    write_file("prompt", "");
    promptRouteIfNeeded();
    wait_for_child();
    assert(g_promptBuilt && access("ready", F_OK) == 0);
    assert(unlink("prompt") == 0);
    g_promptNext = 0;
    promptRouteIfNeeded();
    wait_for_child();
    assert(!g_promptBuilt && access("audio-route.built", F_OK) != 0);

    write_file("prompt", "");
    write_file("slow-build", "");
    g_promptNext = 0;
    promptRouteIfNeeded();
    assert(unlink("prompt") == 0);
    wait_for_child();
    assert(!g_promptBuilt && access("ready", F_OK) != 0);
    assert(access("audio-route.built", F_OK) != 0);
    assert(unlink("slow-build") == 0);

    write_file("prompt", "");
    write_file("fail-build", "");
    write_file("fail-remove", "");
    g_promptNext = 0;
    promptRouteIfNeeded();
    wait_for_child();
    assert(g_promptBuilt && access("audio-route.built", F_OK) == 0);
    assert(access("ready", F_OK) != 0);
    assert(unlink("fail-remove") == 0);
    g_promptNext = 0;
    promptRouteIfNeeded();
    wait_for_child();
    assert(!g_promptBuilt && access("audio-route.built", F_OK) != 0);
    assert(!g_promptChild && g_promptDisabled);
    return 0;
}
