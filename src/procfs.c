#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>

#include "procfs.h"

#define PROC_PATH_LEN 128

static char *find_comm_end(char *buf)
{
    char *end = NULL;
    for (char *p = buf; *p; p++)
    {
        if (*p != ')') continue;
        if (p[1] != ' ') continue;
        if (strchr("RSDZTWXIKP", p[2])) end = p;
    }
    return end;
}

int64_t procfs_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int procfs_read(pid_t pid, uint64_t *cpu_ticks, uint64_t *rss_kb, uint64_t *read_bytes, uint64_t *write_bytes)
{
    char path[PROC_PATH_LEN];
    char buf[8192];
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);

    char *end = find_comm_end(buf);
    if (!end) return -1;

    char *save = NULL;
    char *tok = strtok_r(end + 2, " \t", &save);
    int field = 3;
    uint64_t utime = 0, stime = 0;
    int found_utime = 0, found_stime = 0;

    while (tok)
    {
        if (field == 14) { utime = strtoull(tok, NULL, 10); found_utime = 1; }
        if (field == 15) { stime = strtoull(tok, NULL, 10); found_stime = 1; break; }
        field++;
        tok = strtok_r(NULL, " \t", &save);
    }
    if (!found_utime || !found_stime) return -1;
    *cpu_ticks = utime + stime;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (!f) return -1;
    *rss_kb = 0;
    while (fgets(buf, sizeof(buf), f))
    {
        if (strncmp(buf, "VmRSS:", 6) == 0)
        {
            if (sscanf(buf + 6, "%" SCNu64, rss_kb) == 1) break;
        }
    }
    fclose(f);

    snprintf(path, sizeof(path), "/proc/%d/io", (int)pid);
    f = fopen(path, "r");
    if (!f) { *read_bytes = 0; *write_bytes = 0; return 0; }
    *read_bytes = 0; *write_bytes = 0;
    while (fgets(buf, sizeof(buf), f))
    {
        sscanf(buf, "read_bytes: %" SCNu64, read_bytes);
        sscanf(buf, "write_bytes: %" SCNu64, write_bytes);
    }
    fclose(f);
    return 0;
}

int procfs_cpu_count(void)
{
    static int cores = -1;
    if (cores < 0)
    {
        cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cores <= 0) cores = 1;
    }
    return cores;
}

double procfs_host_cpu_percent(void)
{
    static uint64_t prev_user = 0, prev_nice = 0, prev_system = 0;
    static uint64_t prev_idle = 0, prev_iowait = 0, prev_irq = 0, prev_softirq = 0;
    static int has_prev = 0;

    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;
    char buf[8192];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0.0; }
    fclose(f);

    uint64_t user, nice, system, idle, iowait, irq, softirq;
    if (sscanf(buf, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) return 0.0;

    if (!has_prev)
    {
        prev_user = user; prev_nice = nice; prev_system = system;
        prev_idle = idle; prev_iowait = iowait; prev_irq = irq; prev_softirq = softirq;
        has_prev = 1; return 0.0;
    }

    uint64_t d_user = user - prev_user; uint64_t d_nice = nice - prev_nice; uint64_t d_system = system - prev_system;
    uint64_t d_idle = idle - prev_idle; uint64_t d_iowait = iowait - prev_iowait; uint64_t d_irq = irq - prev_irq;
    uint64_t d_softirq = softirq - prev_softirq;

    prev_user = user; prev_nice = nice; prev_system = system;
    prev_idle = idle; prev_iowait = iowait; prev_irq = irq; prev_softirq = softirq;

    uint64_t total_idle = d_idle + d_iowait;
    uint64_t total_time = d_user + d_nice + d_system + total_idle + d_irq + d_softirq;
    if (total_time == 0) return 0.0;
    return (1.0 - ((double)total_idle / (double)total_time)) * 100.0;
}
