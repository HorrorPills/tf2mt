/* threadmon — per-thread CPU sampler for a macOS process (no root needed for own processes).
 * Usage: threadmon <pid> <interval_ms> <out.csv>
 * Writes rows: t_s,thread,user_ms,sys_ms,name   (cumulative per-thread times; deltas done in analysis)
 * Exits when the process disappears.
 */
#include <libproc.h>
#include <sys/proc_info.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s pid interval_ms out.csv\n", argv[0]); return 2; }
    pid_t pid = atoi(argv[1]);
    useconds_t iv = (useconds_t)atoi(argv[2]) * 1000;
    FILE *f = fopen(argv[3], "w");
    if (!f) { perror(argv[3]); return 1; }
    fprintf(f, "t_s,thread,user_ms,sys_ms,name\n");
    static uint64_t th[4096];
    double t0 = now_s();
    for (;;) {
        int n = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, th, sizeof th);
        if (n <= 0) break;
        n /= (int)sizeof th[0];
        double t = now_s() - t0;
        for (int i = 0; i < n; i++) {
            struct proc_threadinfo pti;
            if (proc_pidinfo(pid, PROC_PIDTHREADINFO, th[i], &pti, sizeof pti) != sizeof pti) continue;
            for (char *c = pti.pth_name; *c; c++) if (*c == ',' || *c == '\n') *c = ' ';
            fprintf(f, "%.3f,%llx,%.3f,%.3f,%s\n", t, (unsigned long long)th[i],
                    pti.pth_user_time / 1e6, pti.pth_system_time / 1e6, pti.pth_name);
        }
        fflush(f);
        usleep(iv);
    }
    fclose(f);
    return 0;
}
