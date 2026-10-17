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
