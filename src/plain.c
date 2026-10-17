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

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <libpq-fe.h>

#include "pgwa.h"
#include "postgres.h"
#include "metrics.h"
#include "plain.h"

static pgwa_session sessions[PGWA_MAX_ROWS];

static void print_query(const char *q)
{
    int len = 0;
    while (*q && len < 40)
    {
        char c = *q++;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        putchar(c); len++;
    }
    if (*q) printf("...");
}

static void print_rate(double bytes_per_sec)
{
    if (bytes_per_sec >= 1024.0 * 1024.0 * 1024.0) printf("%7.1fG", bytes_per_sec / (1024.0 * 1024.0 * 1024.0));
    else if (bytes_per_sec >= 1024.0 * 1024.0) printf("%7.1fM", bytes_per_sec / (1024.0 * 1024.0));
    else if (bytes_per_sec >= 1024.0) printf("%7.1fK", bytes_per_sec / 1024.0);
    else printf("%7.0f", bytes_per_sec);
}

static const char *display_wait_type(const pgwa_session *s)
{
    if (s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_type;
}

static const char *display_wait_event(const pgwa_session *s)
{
    if (s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_event;
}

static pgwa_session *find_session(pid_t pid, int session_count)
{
    for (int i = 0; i < session_count; i++) if (sessions[i].pid == pid) return &sessions[i];
    return NULL;
}

static int session_has_blocker(pid_t pid, int session_count)
{
    pgwa_session *s = find_session(pid, session_count);
    return (s != NULL && s->blocker_count > 0);
}

static int session_is_blocker(pid_t pid, int session_count)
{
    for (int i = 0; i < session_count; i++)
        for (int j = 0; j < sessions[i].blocker_count; j++)
            if (sessions[i].blockers[j] == pid) return 1;
    return 0;
}

static int already_visited(pid_t pid, pid_t *visited, int visited_count)
{
    for (int i = 0; i < visited_count; i++) if (visited[i] == pid) return 1;
    return 0;
}

static void print_tree_node(pid_t pid, int depth, int session_count, pid_t *visited, int *visited_count, const char *prefix, int is_last)
{
    pgwa_session *s = find_session(pid, session_count);
    if (s == NULL || already_visited(pid, visited, *visited_count)) return;

    if (*visited_count < PGWA_MAX_ROWS) {
        visited[*visited_count] = pid; (*visited_count)++;
    }

    if (depth > 0) printf("%s%s ", prefix, is_last ? "└─" : "├─");
    printf("%d  %-20.20s", (int)s->pid, s->state);

    if (s->wait_type[0] != '\0') {
        printf(" [%s", s->wait_type);
        if (s->wait_event[0] != '\0') printf(" / %s]", s->wait_event);
        else printf("]");
    }
    putchar('\n');

    int child_count = 0;
    for (int i = 0; i < session_count; i++) {
        pgwa_session *child = &sessions[i];
        for (int j = 0; j < child->blocker_count; j++) {
            if (child->blockers[j] == pid) { child_count++; break; }
        }
    }

    int child_index = 0;
    for (int i = 0; i < session_count; i++) {
        pgwa_session *child = &sessions[i];
        int is_child = 0;
        for (int j = 0; j < child->blocker_count; j++) {
            if (child->blockers[j] == pid) { is_child = 1; break; }
        }
        if (!is_child) continue;

        int child_is_last = (child_index == child_count - 1);
        char next_prefix[4096]; next_prefix[0] = '\0';

        if (depth > 0) snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix, is_last ? "   " : "│  ");
        else snprintf(next_prefix, sizeof(next_prefix), "%s", prefix);

        print_tree_node(child->pid, depth + 1, session_count, visited, visited_count, next_prefix, child_is_last);
        child_index++;
    }
}

static void print_blocking_tree(int session_count)
{
    pid_t visited[PGWA_MAX_ROWS]; int visited_count = 0, printed_tree = 0;
    printf("\nBLOCKING TREE\n-------------\n");

    for (int i = 0; i < session_count; i++) {
        pgwa_session *s = &sessions[i];
        if (!session_is_blocker(s->pid, session_count) || session_has_blocker(s->pid, session_count) || already_visited(s->pid, visited, visited_count)) continue;
        print_tree_node(s->pid, 0, session_count, visited, &visited_count, "", 1);
        printed_tree = 1;
    }
    if (!printed_tree) {
        for (int i = 0; i < session_count; i++) {
            pgwa_session *s = &sessions[i];
            if (session_is_blocker(s->pid, session_count) && !already_visited(s->pid, visited, visited_count)) {
                print_tree_node(s->pid, 0, session_count, visited, &visited_count, "", 1); printed_tree = 1;
            }
        }
    }
    if (!printed_tree) printf("(no blocking chains)\n");
}

void plain_run(const char *port)
{
    pgwa_pg_connection pg = {0}; int session_count = 0;
    if (postgres_connect(&pg, port) != 0) { fprintf(stderr, "Cannot connect to PostgreSQL\n"); return; }

    while (1)
    {
        if (postgres_read_activity(&pg, sessions, PGWA_MAX_ROWS, &session_count) != 0) break;
        metrics_update(sessions, session_count);
        metrics_update_tps(pg.total_xacts);

        printf("\033[2J\033[H");
        char *p_port = PQport((PGconn *)pg.conn); char *p_db = PQdb((PGconn *)pg.conn);
        char *p_user = PQuser((PGconn *)pg.conn);
        int v_raw = PQserverVersion((PGconn *)pg.conn);
        int v_maj = v_raw / 10000; int v_min = (v_raw % 10000) / 100;

        printf("pg_whoisactive | Server: port=%s db=%s user=%s v%d.%d | TPS: %.0f | Sessions: %d\n\n",
               p_port ? p_port : "-", p_db ? p_db : "-", p_user ? p_user : "-", v_maj, v_min, metrics_get_tps(), session_count);

        printf("%-6s %-10s %-8s %5s %7s %7s %5s %-8s %5s %6s %-8s %-12s %s\n",
               "PID", "USER", "DB", "CPU", "READ/s", "WRITE/s", "RSS", "STATE", "TIME", "BLK", "WAIT", "EVENT", "QUERY");
        printf("-----------------------------------------------------------------------------------------------------------------------------------------\n");

        for (int i = 0; i < session_count; i++)
        {
            pgwa_session *s = &sessions[i];
            if (s->leader_pid != 0) continue; 

            printf("%-6d %-10.10s %-8.8s %5llu ", (int)s->pid, s->user, s->database, (unsigned long long)s->cpu_ms);
            print_rate(s->read_per_sec); putchar(' ');
            print_rate(s->write_per_sec); putchar(' ');
            printf("%4lluM %-8.8s %5.0fs %6d %-8.8s %-12.12s ", 
                   (unsigned long long)(s->rss_kb / 1024), 
                   s->state, 
                   s->runtime_sec, 
                   (int)s->blocker_pid, 
                   display_wait_type(s), 
                   display_wait_event(s));
            print_query(s->query); 
            putchar('\n');

            // parallel workers
            for (int j = 0; j < session_count; j++) {
                if (sessions[j].leader_pid == s->pid) {
                    pgwa_session *w = &sessions[j];
                    
                    printf("└─%-4d %-10s %-8s %5llu ", (int)w->pid, "worker", "", (unsigned long long)w->cpu_ms);
                    print_rate(w->read_per_sec); putchar(' ');
                    print_rate(w->write_per_sec); putchar(' ');
                    printf("%4lluM %-8s %5s %6s %-8s %-12s\n", 
                           (unsigned long long)(w->rss_kb / 1024), "", "", "", "", "");
                }
            }
        }
        print_blocking_tree(session_count); fflush(stdout); sleep(1);
    }
    postgres_disconnect(&pg);
}

void plain_snapshot(const char *port)
{
    pgwa_pg_connection pg = {0}; int session_count = 0;
    if (postgres_connect(&pg, port) != 0) { fprintf(stderr, "Cannot connect to PostgreSQL\n"); return; }

    /* First sample — just to populate the metrics cache */
    if (postgres_read_activity(&pg, sessions, PGWA_MAX_ROWS, &session_count) != 0) {
        postgres_disconnect(&pg);
        return;
    }
    metrics_update(sessions, session_count);
    metrics_update_tps(pg.total_xacts);

    /* Wait ~1s and take the second sample — now with deltas*/
    sleep(1);

    if (postgres_read_activity(&pg, sessions, PGWA_MAX_ROWS, &session_count) != 0) {
        postgres_disconnect(&pg);
        return;
    }
    metrics_update(sessions, session_count);
    metrics_update_tps(pg.total_xacts);

    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    char *p_port = PQport((PGconn *)pg.conn); char *p_db = PQdb((PGconn *)pg.conn);
    char *p_user = PQuser((PGconn *)pg.conn);
    int v_raw = PQserverVersion((PGconn *)pg.conn);
    int v_maj = v_raw / 10000; int v_min = (v_raw % 10000) / 100;

    printf("=== pg_whoisactive snapshot %s ===\n", tbuf);
    printf("Server: port=%s db=%s user=%s v%d.%d | TPS: %.0f | Sessions: %d\n\n",
           p_port ? p_port : "-", p_db ? p_db : "-", p_user ? p_user : "-", v_maj, v_min, metrics_get_tps(), session_count);

    printf("%-6s %-10s %-8s %5s %7s %7s %5s %-8s %5s %6s %-8s %-12s %s\n",
           "PID", "USER", "DB", "CPU", "READ/s", "WRITE/s", "RSS", "STATE", "TIME", "BLK", "WAIT", "EVENT", "QUERY");
    printf("----------------------------------------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < session_count; i++)
    {
        pgwa_session *s = &sessions[i];
        if (s->leader_pid != 0) continue;

        printf("%-6d %-10.10s %-8.8s %5llu ", (int)s->pid, s->user, s->database, (unsigned long long)s->cpu_ms);
        print_rate(s->read_per_sec); putchar(' ');
        print_rate(s->write_per_sec); putchar(' ');
        printf("%4lluM %-8.8s %5.0fs %6d %-8.8s %-12.12s ", 
               (unsigned long long)(s->rss_kb / 1024), 
               s->state, 
               s->runtime_sec, 
               (int)s->blocker_pid, 
               display_wait_type(s), 
               display_wait_event(s));
        print_query(s->query); 
        putchar('\n');

        for (int j = 0; j < session_count; j++) {
            if (sessions[j].leader_pid == s->pid) {
                pgwa_session *w = &sessions[j];
                printf("└─%-4d %-10s %-8s %5llu ", (int)w->pid, "worker", "", (unsigned long long)w->cpu_ms);
                print_rate(w->read_per_sec); putchar(' ');
                print_rate(w->write_per_sec); putchar(' ');
                printf("%4lluM %-8s %5s %6s %-8s %-12s\n", 
                       (unsigned long long)(w->rss_kb / 1024), "", "", "", "", "");
            }
        }
    }
    print_blocking_tree(session_count);
    printf("\n");
    postgres_disconnect(&pg);
}
