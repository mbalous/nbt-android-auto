#ifndef NBT_AA_SERVICE_LOCK_H
#define NBT_AA_SERVICE_LOCK_H

/* Keep the guard inode for the lifetime of the RAM directory: unlinking it
 * would allow another process to lock a different inode at the same path. */
static int acquire_service_lock(int* recovered)
{
    struct flock guard;
    FILE* previous;
    int fd, marker, pid = 0;
    char text[32], extra;
    int n;

    fd = open("service.guard", O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return -1;
    memset(&guard, 0, sizeof(guard));
    guard.l_type = F_WRLCK;
    guard.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &guard) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }

    previous = fopen("service.lock", "r");
    if (previous) {
        /* An empty or invalid legacy lock is ambiguous; never displace it. */
        if (fscanf(previous, "%d %c", &pid, &extra) != 1 || pid <= 1) {
            fclose(previous);
            close(fd);
            errno = EBUSY;
            return -1;
        }
        fclose(previous);
        if (kill(pid, 0) == 0 || errno != ESRCH) {
            close(fd);
            errno = EBUSY;
            return -1;
        }
        *recovered = pid;
    } else if (errno != ENOENT) {
        close(fd);
        return -1;
    }

    marker = open("service.lock", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    n = snprintf(text, sizeof(text), "%d\n", (int)getpid());
    if (marker < 0 || write(marker, text, (size_t)n) != n) {
        if (marker >= 0) {
            close(marker);
            unlink("service.lock");
        }
        close(fd);
        return -1;
    }
    close(marker);
    return fd;
}

#endif
