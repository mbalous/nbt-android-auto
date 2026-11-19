#define main native_service_main
#include "nbt_native_service.c"
#undef main

int main(void)
{
    audio_route_cleanup();
    set_audio_mode();
    printf("test mode=%s\n", getenv("NBT_AUDIO_MODE"));
    return 0;
}
