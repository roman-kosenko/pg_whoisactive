/*
 * pg_whoisactive (pgwa)
 * Copyright (C) 2026 Roman Kosenko
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
