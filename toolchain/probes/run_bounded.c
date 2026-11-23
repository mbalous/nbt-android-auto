/* Run an offline VM test with a bounded lifetime. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BOUND_SECONDS
#define BOUND_SECONDS 30
#endif

static volatile sig_atomic_t timed_out;

static void timeout(int sig) {
    (void)sig;
    timed_out = 1;
}

int main(int argc, char **argv) {
    pid_t child, waited;
    struct timespec delay = {0, 10000000};
    int status;
    if (argc < 2) return 2;
    child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (!child) {
        execv(argv[1], argv + 1);
        perror("execv");
        _exit(127);
    }
    signal(SIGALRM, timeout);
    alarm(BOUND_SECONDS);
    for (;;) {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno != EINTR)) break;
        if (timed_out) {
            if (kill(child, SIGKILL) < 0 && errno != ESRCH) {
                perror("kill");
                return 1;
            }
            do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
            break;
        }
        nanosleep(&delay, NULL);
    }
    alarm(0);
    if (timed_out || waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("bounded test failed or timed out\n", stderr);
        return 1;
    }
    return 0;
}
