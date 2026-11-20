#define main native_service_main
#include "nbt_native_service.c"
#undef main

int main(void)
{
    audio_route_cleanup();
    set_audio_mode();
    printf("test mode=%s\n", getenv("NBT_AUDIO_MODE"));
    printf("test media=%s source=%s prompts=%s\n",
           getenv("NBT_AA_MEDIA_OUT") ? getenv("NBT_AA_MEDIA_OUT") : "(unset)",
           getenv("NBT_AUX_SOURCE"), getenv("NBT_PROMPT_CONN"));
    return 0;
}
