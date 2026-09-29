#include <unistd.h>
#include <stdint.h>
#include <math.h>

#include "metrics.h"
#include "procfs.h"

#define CACHE_SIZE 1024
#define TPS_WINDOW_SIZE 5

typedef struct
{
    pid_t pid;

    uint64_t cpu_ticks;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t rss_kb; // Добавлено поле кэша памяти

    int64_t sample_time_ns;

    int valid;

} metrics_cache;

static metrics_cache cache[CACHE_SIZE];
static metrics_info global_metrics = {0.0, 0.0, 0};

/* Переменные скользящего окна для расчета TPS */
static tps_history_point tps_history[TPS_WINDOW_SIZE];
static int tps_history_idx = 0;
static double current_tps = 0.0;

static metrics_cache *find_slot(pid_t pid)
{
    for (int i = 0; i < CACHE_SIZE; i++)
    {
        if (cache[i].valid && cache[i].pid == pid)
            return &cache[i];
    }

    for (int i = 0; i < CACHE_SIZE; i++)
    {
        if (!cache[i].valid)
        {
            cache[i].valid = 1;
            cache[i].pid = pid;
            return &cache[i];
        }
    }

    return &cache[pid % CACHE_SIZE];
}

void metrics_update(pgwa_session *sessions,
                    int session_count)
{
    const long hz = sysconf(_SC_CLK_TCK);
    const int cores = procfs_cpu_count();

    if (hz <= 0 || cores <= 0)
        return;

    const int64_t now = procfs_now_ns();

    if (now <= 0)
        return;

    double total_pg_cpu_pct = 0.0;

    for (int i = 0; i < session_count; i++)
    {
        pgwa_session *s = &sessions[i];

        uint64_t ticks;
        uint64_t rss;
        uint64_t rd;
        uint64_t wr;

        if (procfs_read(s->pid,
                        &ticks,
                        &rss,
                        &rd,
                        &wr) != 0)
        {
            s->cpu_valid = 0;
            s->io_valid = 0;
            s->cpu_ms = 0;
            s->cpu_pct = 0.0;
            s->rss_delta_kb = 0;

            continue;
        }

        s->rss_kb = rss;

        metrics_cache *c = find_slot(s->pid);

        /*
         * First sample for this PID.
         */
        if (c->sample_time_ns == 0)
        {
            c->cpu_ticks = ticks;
            c->read_bytes = rd;
            c->write_bytes = wr;
            c->rss_kb = rss; // Сохраняем первый замер памяти
            c->sample_time_ns = now;

            s->cpu_ms = 0;
            s->cpu_pct = 0.0;
            s->rss_delta_kb = 0;
            s->cpu_valid = 0;
            s->io_valid = 0;

            continue;
        }

        /*
         * PID reuse or counter reset.
         */
        if (ticks < c->cpu_ticks ||
            rd < c->read_bytes ||
            wr < c->write_bytes ||
            now <= c->sample_time_ns)
        {
            c->cpu_ticks = ticks;
            c->read_bytes = rd;
            c->write_bytes = wr;
            c->rss_kb = rss;
            c->sample_time_ns = now;

            s->cpu_ms = 0;
            s->cpu_pct = 0.0;
            s->rss_delta_kb = 0;
            s->cpu_valid = 0;
            s->io_valid = 0;

            continue;
        }

        const uint64_t cpu_delta =
            ticks - c->cpu_ticks;

        const uint64_t read_delta =
            rd - c->read_bytes;

        const uint64_t write_delta =
            wr - c->write_bytes;

        /*
         * Convert process CPU ticks to milliseconds.
         */
        s->cpu_ms =
            (cpu_delta * 1000ULL) / (uint64_t)hz;

        const int64_t elapsed_ns =
            now - c->sample_time_ns;

        const double elapsed_sec =
            (double)elapsed_ns / 1000000000.0;

        if (elapsed_sec >= 0.5)
        {
            double process_cpu_sec = (double)cpu_delta / (double)hz;
            s->cpu_pct = (process_cpu_sec / (elapsed_sec * (double)cores)) * 100.0;

            // Расчет честной знаковой дельты изменения оперативной памяти
            s->rss_delta_kb = (int64_t)rss - (int64_t)c->rss_kb;

            s->read_per_sec =
                (double)read_delta / elapsed_sec;

            s->write_per_sec =
                (double)write_delta / elapsed_sec;

            s->cpu_valid = 1;
            s->io_valid = 1;

            total_pg_cpu_pct += s->cpu_pct;
        }
        else
        {
            s->cpu_pct = 0.0; 
            s->rss_delta_kb = 0;
        }

        c->cpu_ticks = ticks;
        c->read_bytes = rd;
        c->write_bytes = wr;
        c->rss_kb = rss; // Обновляем исторический кадр памяти для следующей итерации
        c->sample_time_ns = now;
    }

    /* Расчет глобальных показателей CPU для верхней панели */
    global_metrics.cpu_cores = cores;
    global_metrics.host_cpu_pct = procfs_host_cpu_percent();

    if (total_pg_cpu_pct > global_metrics.host_cpu_pct)
    {
        total_pg_cpu_pct = global_metrics.host_cpu_pct;
    }

    if (total_pg_cpu_pct > 100.0)
        total_pg_cpu_pct = 100.0;
        
    global_metrics.pg_cpu_pct = total_pg_cpu_pct;
}

void metrics_update_tps(uint64_t curr_total_xacts)
{
    int64_t now = procfs_now_ns();
    if (now <= 0) return;

    if (!tps_history[tps_history_idx].valid)
    {
        for (int i = 0; i < TPS_WINDOW_SIZE; i++)
        {
            tps_history[i].total_xacts = curr_total_xacts;
            tps_history[i].time_ns = now;
            tps_history[i].valid = 1;
        }
        current_tps = 0.0;
        return;
    }

    if (curr_total_xacts < tps_history[tps_history_idx].total_xacts || now <= tps_history[tps_history_idx].time_ns)
    {
        for (int i = 0; i < TPS_WINDOW_SIZE; i++)
        {
            tps_history[i].total_xacts = curr_total_xacts;
            tps_history[i].time_ns = now;
            tps_history[i].valid = 1;
        }
        current_tps = 0.0;
        return;
    }

    uint64_t delta_xacts = curr_total_xacts - tps_history[tps_history_idx].total_xacts;
    int64_t elapsed_ns = now - tps_history[tps_history_idx].time_ns;
    double elapsed_sec = (double)elapsed_ns / 1000000000.0;

    if (elapsed_sec > 0.1)
    {
        current_tps = (double)delta_xacts / elapsed_sec;
    }

    tps_history[tps_history_idx].total_xacts = curr_total_xacts;
    tps_history[tps_history_idx].time_ns = now;
    tps_history_idx = (tps_history_idx + 1) % TPS_WINDOW_SIZE;
}

double metrics_get_tps(void) { return current_tps; }
metrics_info metrics_get_global(void) { return global_metrics; }
