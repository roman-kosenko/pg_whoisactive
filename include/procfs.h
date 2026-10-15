#ifndef PROCFS_H
#define PROCFS_H

#include "pgwa.h"

int procfs_read_v2(pid_t pid,
                pid_t leader_pid,
                uint64_t *cpu_ticks,
                uint64_t *rss_kb,
                uint64_t *read_bytes,
                uint64_t *write_bytes);

int64_t procfs_now_ns(void);

int procfs_cpu_count(void);
double procfs_host_cpu_percent(void);

#endif
