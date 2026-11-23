/* Load a trusted plugin against fail-fast synthetic HMI libraries; never call its factory. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct plugin_info {
    uint16_t major, minor;
    const char *name, *vendor;
    void (*create)(void);
    void (*destroy)(void);
};

int main(int argc, char **argv) {
    int i;
    if (argc != 2) return 2;
    alarm(10);
    for (i = 0; i < 2; ++i) {
        void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        struct plugin_info *info;
        if (!handle) {
            fprintf(stderr, "dlopen failed: %s\n", dlerror());
            return 1;
        }
        info = (struct plugin_info *)dlsym(handle, "pluginAppInfo");
        if (!info || !info->name || !info->vendor || info->major != 1 || info->minor != 0 ||
            strcmp(info->name, "NbtAndroidAuto") ||
            strcmp(info->vendor, "NBT-Project") || !info->create || !info->destroy) {
            fputs("pluginAppInfo mismatch\n", stderr);
            return 1;
        }
        if (dlclose(handle) != 0) {
            fprintf(stderr, "dlclose failed: %s\n", dlerror());
            return 1;
        }
    }
    puts("HMI_LOADER_OK repeated-load metadata");
    return 0;
}
