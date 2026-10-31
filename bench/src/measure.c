/* measure: fork/exec a command, print wall/user/sys/maxrss in one parseable line
   to stderr (used when /usr/bin/time is unavailable; also gives uniform output). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: measure cmd [args]\n"); return 2; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t p = fork();
    if (p == 0) { execvp(argv[1], argv + 1); perror("exec"); _exit(127); }
    int st; struct rusage ru; wait4(p, &st, 0, &ru);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double us = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, sy = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    fprintf(stderr, "MEASURE wall=%.3f user=%.3f sys=%.3f maxrss_kb=%ld cpu_pct=%.1f exit=%d\n",
            wall, us, sy, ru.ru_maxrss, wall > 0 ? 100.0 * (us + sy) / wall : 0.0,
            WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
