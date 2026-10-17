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

#ifndef METRICS_H
#define METRICS_H

#include "pgwa.h"

/*Structure to store a single transaction history snapshot in a circular buffer*/
typedef struct {
    uint64_t total_xacts;
    int64_t time_ns;
    int valid;
} tps_history_point;

/* Global structure for passing CPU metrics to the UI */
typedef struct {
    double host_cpu_pct;
    double pg_cpu_pct;
    int cpu_cores;
} metrics_info;

void metrics_update(pgwa_session *sessions, int session_count);
void metrics_update_tps(uint64_t curr_total_xacts);

double metrics_get_tps(void);
metrics_info metrics_get_global(void);

#endif
