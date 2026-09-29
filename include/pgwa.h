#ifndef PGWA_H
#define PGWA_H

#include <stdint.h>
#include <sys/types.h>

#define PGWA_MAX_QUERY      16384
#define PGWA_MAX_USER       64
#define PGWA_MAX_HOST       64
#define PGWA_MAX_DB         64
#define PGWA_MAX_APP        128
#define PGWA_MAX_WAIT       64
#define PGWA_MAX_STARTED    32
#define PGWA_MAX_ROWS       512
#define PGWA_MAX_BLOCKERS   16

typedef enum
{
    SORT_CPU,
    SORT_RUNTIME,
    SORT_READ,
    SORT_WRITE,
    SORT_LOCK
} pgwa_sort_mode;

typedef struct
{
    pid_t pid;

    char user[PGWA_MAX_USER];
    char host[PGWA_MAX_HOST];
    char database[PGWA_MAX_DB];
    char application[PGWA_MAX_APP];

    char wait_type[PGWA_MAX_WAIT];
    char wait_event[PGWA_MAX_WAIT];

    char state[32]; 

    /*
     * Timestamp of the current/last query as reported
     * by pg_stat_activity.query_start.
     */
    char query_start[PGWA_MAX_STARTED];

    char query[PGWA_MAX_QUERY];

    double runtime_sec; 
    double xact_age_sec;  // Новое поле: возраст текущей транзакции
    double query_age_sec; // Новое поле: возраст текущего запроса

    uint64_t cpu_ms;
    double cpu_pct;

    uint64_t rss_kb;
    int64_t rss_delta_kb;

    uint64_t read_bytes;
    uint64_t write_bytes;

    double read_per_sec;
    double write_per_sec;

    pid_t blocker_pid;

    pid_t blockers[PGWA_MAX_BLOCKERS];
    int blocker_count;

    int cpu_valid;
    int io_valid;

} pgwa_session;

typedef struct
{
    pid_t pid;

    uint64_t cpu_ticks;
    uint64_t read_bytes;
    uint64_t write_bytes;

    int64_t sample_time_ms;

} pgwa_proc_sample;

#endif
