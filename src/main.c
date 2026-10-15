#include <stdio.h>
#include <string.h>

#include "plain.h"
#include "ui.h"

#ifndef VERSION
#define VERSION "dev"
#endif

static void print_usage(const char *program)
{
    printf(
        "pg_whoisactive — tool for finding and diagnosing PostgreSQL problems\n"
        "\n"
        "Usage:\n"
        "  %s [port] [options]\n"
        "  pgwa [port] [options]          (symlink created by the package installer)\n"
        "\n"
        "Interactive mode (default):\n"
        "  Real-time ncurses interface showing active sessions, blocking chains,\n"
        "  CPU/IO/memory metrics and query text. Color-coded by state:\n"
        "    green  — active\n"
        "    yellow — waiting on lock\n"
        "    red    — aborted\n"
        "\n"
        "  Keys:\n"
        "    Up/Down              Select row\n"
        "    t/c/m/r/w/p          Sort by Time / CPU%% / Memory (RSS) / READ/s / WRITE/s / PID\n"
        "    f                    Filter by user\n"
        "    k                    Kill/Cancel selected session\n"
        "                         (selecting a parallel worker acts on its leader)\n"
        "    Space                Pause / resume refresh\n"
        "    PgUp/PgDn            Scroll query panel\n"
        "    q                    Quit\n"
        "\n"
        "Options:\n"
        "  --plain              Continuous plain-text output (refreshes every 1s)\n"
        "  --snapshot, -s       Print one snapshot and exit (suitable for cron/logging)\n"
        "  --version, -v        Show version and exit\n"
        "  --help, -h           Show this help\n"
        "\n"
        "Arguments:\n"
        "  port                 PostgreSQL port (default: 5432)\n"
        "\n"
        "Examples:\n"
        "  sudo -u postgres %s\n"
        "  sudo -u postgres pgwa 5433\n"
        "  %s --plain\n"
        "  sudo -u postgres pgwa --snapshot >> /tmp/pgwa.log\n"
        "\n"
        "Notes:\n"
        "  - Connects via local Unix socket by default (no password when run as the postgres OS user).\n"
        "  - Recommended: sudo -u postgres %s\n"
        "  - Needs permission to read /proc of PostgreSQL backends for CPU/IO/memory metrics.\n"
        "  - --snapshot takes two samples ~1 second apart to compute rates.\n"
        "  - Parallel workers appear indented; kill on a worker acts on its leader.\n"
        "\n"
        "Header metrics:\n"
        "  Host CPU — total CPU usage of the machine\n"
        "  PG CPU   — sum of CPU%% of all PostgreSQL backends (capped at Host CPU)\n"
        "  CPUs     — number of CPU cores\n"
        "  TPS      — transactions per second (commits + rollbacks across all DBs)\n"
        "  AV       — autovacuum workers currently running / autovacuum_max_workers\n"
        "  Sessions — total number of client sessions\n"
        "\n"
        "Per-session metrics:\n"
        "  CPUs%%   — CPU usage of this process over the last sample (all cores = 100%%)\n"
        "  CPUms    — CPU milliseconds consumed by this process in the last sample\n"
        "  READ/s   — disk bytes read per second\n"
        "  WRITE/s  — disk bytes written per second\n"
        "  RSS      — current resident set size\n"
        "  dRSS     — change in RSS since the previous sample\n"
        "\n"
        "  Rates need two samples. Interactive/--plain show them from the second\n"
        "  refresh. --snapshot collects both samples internally before printing.\n",
        program, program, program, program
    );
}

int main(int argc, char **argv)
{
    // МГНОВЕННЫЙ ПЕРЕХВАТ: Проверяем флаги версии до любого анализа портов и режимов
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
        {
            printf("pg_whoisactive v:%s\n", VERSION);
            return 0; // Чистый выход с кодом 0, как требует задача
        }
    }

    int plain_mode = 0;
    int snapshot_mode = 0;
    const char *port = NULL;

    // Основной цикл обработки рабочих параметров (порт, plain-режим)
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--plain") == 0)
        {
            plain_mode = 1;
        }
        else if (strcmp(argv[i], "--snapshot") == 0 || strcmp(argv[i], "-s") == 0)
        {
            snapshot_mode = 1;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            if (port == NULL)
            {
                port = argv[i];
            }
            else
            {
                fprintf(stderr, "Unknown option or multiple ports specified: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    if (snapshot_mode)
    {
        plain_snapshot(port);
    }
    else if (plain_mode)
    {
        plain_run(port);
    }
    else
    {
        ui_run(port);
    }

    return 0;
}
