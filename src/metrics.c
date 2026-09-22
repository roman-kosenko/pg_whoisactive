#include <unistd.h>
#include <stdint.h>

#include "metrics.h"
#include "procfs.h"

#define CACHE_SIZE 1024

typedef struct
{
    pid_t pid;

    uint64_t cpu_ticks;
    uint64_t read_bytes;
    uint64_t write_bytes;

    int64_t sample_time_ns;

    int valid;

} metrics_cache;

static metrics_cache cache[CACHE_SIZE];

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

    if (hz <= 0)
        return;

    const int64_t now = procfs_now_ns();

    if (now <= 0)
        return;

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
            c->sample_time_ns = now;

            s->cpu_ms = 0;
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
            c->sample_time_ns = now;

            s->cpu_ms = 0;
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
         *
         * CPU time is independent of wall-clock elapsed time.
         */
        s->cpu_ms =
            (cpu_delta * 1000ULL) / (uint64_t)hz;

        const int64_t elapsed_ns =
            now - c->sample_time_ns;

        const double elapsed_sec =
            (double)elapsed_ns / 1000000000.0;

        if (elapsed_sec >= 0.5)
        {
            s->read_per_sec =
                (double)read_delta / elapsed_sec;

            s->write_per_sec =
                (double)write_delta / elapsed_sec;

            s->cpu_valid = 1;
            s->io_valid = 1;
        }

        c->cpu_ticks = ticks;
        c->read_bytes = rd;
        c->write_bytes = wr;
        c->sample_time_ns = now;
    }
}