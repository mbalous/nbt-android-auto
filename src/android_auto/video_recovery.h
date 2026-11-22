#ifndef NBT_VIDEO_RECOVERY_H
#define NBT_VIDEO_RECOVERY_H

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#ifndef NBT_VIDEO_CONTROL
#define NBT_VIDEO_CONTROL "/fs/ram3/nbt-aa-native/video.control"
#endif
#ifndef NBT_VIDEO_STATUS
#define NBT_VIDEO_STATUS "/fs/ram3/nbt-aa-native/video.status"
#endif

static int video_recovery_publish(const char* path, const char* token, unsigned generation, const char* state)
{
    char tmp[256], line[160];
    int fd, length, ok;
    if (snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid()) >= (int)sizeof(tmp))
        return 0;
    length = snprintf(line, sizeof(line), "%s %u %s\n", token, generation, state);
    if (length <= 0 || length >= (int)sizeof(line))
        return 0;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    ok = write(fd, line, length) == length;
    if (close(fd))
        ok = 0;
    if (ok && rename(tmp, path) == 0)
        return 1;
    unlink(tmp);
    return 0;
}

static int video_recovery_read(const char* path, const char* token, unsigned* generation,
                               char* state, size_t state_size)
{
    char found[64], value[32], extra;
    unsigned n;
    FILE* f = fopen(path, "r");
    int ok;
    if (!f)
        return 0;
    ok = fscanf(f, "%63s %u %31s %c", found, &n, value, &extra) == 3;
    fclose(f);
    if (!ok || strcmp(found, token) || strlen(value) >= state_size)
        return 0;
    *generation = n;
    strcpy(state, value);
    return 1;
}

#endif
