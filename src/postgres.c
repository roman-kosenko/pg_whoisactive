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
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <libpq-fe.h>

#include "pgwa.h"
#include "postgres.h"

int postgres_connect(pgwa_pg_connection *pg, const char *port)
{
    PGconn *conn;
    char conninfo[256];
    if (pg == NULL) return -1;

    if (port != NULL && port[0] != '\0') {
        snprintf(conninfo, sizeof(conninfo), "host=/var/run/postgresql port=%s", port);
        conn = PQconnectdb(conninfo);
        if (PQstatus(conn) != CONNECTION_OK) {
            PQfinish(conn);
            snprintf(conninfo, sizeof(conninfo), "host=/tmp port=%s", port);
            conn = PQconnectdb(conninfo);
        }
    } else {
        conn = PQconnectdb("");
    }

    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "PostgreSQL connection failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return -1;
    }
    if (PQsetClientEncoding(conn, "UTF-8") != 0) {
        fprintf(stderr, "Warning: Could not set client encoding to UTF-8.\n");
    }
    pg->conn = conn;
    return 0;
}

void postgres_disconnect(pgwa_pg_connection *pg)
{
    if (pg == NULL || pg->conn == NULL) return;
    PQfinish((PGconn *)pg->conn);
    pg->conn = NULL;
}

int postgres_execute(pgwa_pg_connection *pg, const char *sql)
{
    PGconn *conn; PGresult *res;
    if (pg == NULL || pg->conn == NULL || sql == NULL) return -1;
    conn = (PGconn *)pg->conn;
    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK && PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res); return -1;
    }
    PQclear(res); return 0;
}

int postgres_read_activity(pgwa_pg_connection *pg, pgwa_session *sessions, int max_sessions, int *session_count)
{
    PGconn *conn; PGresult *res;
    if (pg == NULL || pg->conn == NULL || sessions == NULL || session_count == NULL || max_sessions <= 0) return -1;
    conn = (PGconn *)pg->conn;

    pg->av_active = 0;
    pg->av_max = 3;

    PGresult *res_stats = PQexec(conn, 
        "SELECT "
        "  ( (SELECT COALESCE(SUM(xact_commit + xact_rollback), 0) FROM pg_stat_database WHERE datname NOT IN ('template0','template1')) "
        "    - (COALESCE(pg_stat_get_db_xact_commit(0), 0) + COALESCE(pg_stat_get_db_xact_rollback(0), 0)) ), "
        "  (SELECT count(*)::int FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'), "
        "  current_setting('autovacuum_max_workers')::int;");
    
    if (PQresultStatus(res_stats) == PGRES_TUPLES_OK && PQntuples(res_stats) > 0)
    {
        pg->total_xacts = strtoull(PQgetvalue(res_stats, 0, 0), NULL, 10);
        pg->av_active   = atoi(PQgetvalue(res_stats, 0, 1));
        pg->av_max      = atoi(PQgetvalue(res_stats, 0, 2));
        if (pg->av_max <= 0) pg->av_max = 1;
    }
    PQclear(res_stats);

    // backend_type IN ('client backend', 'parallel worker') 
    const char *sql =
        "SELECT pid, COALESCE(usename, ''), COALESCE(client_addr::text, ''), COALESCE(datname, ''), "
        "COALESCE(application_name, ''), COALESCE(state, ''), COALESCE(wait_event_type, ''), COALESCE(wait_event, ''), "
        "CASE WHEN state = 'active' THEN COALESCE(EXTRACT(EPOCH FROM (clock_timestamp() - query_start)), 0) "
        "ELSE COALESCE(EXTRACT(EPOCH FROM (clock_timestamp() - state_change)), 0) END, "
        "COALESCE(to_char(query_start, 'YYYY-MM-DD HH24:MI:SS'), ''), COALESCE(query, ''), "
        "COALESCE(array_to_string(pg_blocking_pids(pid), ','), ''), "
        "CASE WHEN xact_start IS NOT NULL THEN COALESCE(EXTRACT(EPOCH FROM (clock_timestamp() - xact_start)), 0) ELSE 0 END, "
        "CASE WHEN query_start IS NOT NULL THEN COALESCE(EXTRACT(EPOCH FROM (clock_timestamp() - query_start)), 0) ELSE 0 END, "
        "COALESCE(leader_pid, 0) "
        "FROM pg_stat_activity WHERE backend_type IN ('client backend', 'parallel worker') AND pid <> pg_backend_pid() "
        "ORDER BY CASE WHEN state = 'active' THEN 0 ELSE 1 END, query_start NULLS LAST";

    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_stat_activity query failed: %s\n", PQerrorMessage(conn));
        PQclear(res); return -1;
    }

    int rows = PQntuples(res);
    if (rows > max_sessions) rows = max_sessions;

    for (int i = 0; i < rows; i++) {
        pgwa_session *s = &sessions[i]; memset(s, 0, sizeof(*s));
        s->pid = (pid_t)strtol(PQgetvalue(res, i, 0), NULL, 10);
        snprintf(s->user, sizeof(s->user), "%s", PQgetvalue(res, i, 1));
        snprintf(s->host, sizeof(s->host), "%s", PQgetvalue(res, i, 2));
        snprintf(s->database, sizeof(s->database), "%s", PQgetvalue(res, i, 3));
        snprintf(s->application, sizeof(s->application), "%s", PQgetvalue(res, i, 4));
        snprintf(s->state, sizeof(s->state), "%s", PQgetvalue(res, i, 5));
        snprintf(s->wait_type, sizeof(s->wait_type), "%s", PQgetvalue(res, i, 6));
        snprintf(s->wait_event, sizeof(s->wait_event), "%s", PQgetvalue(res, i, 7));
        s->runtime_sec = strtod(PQgetvalue(res, i, 8), NULL);
        snprintf(s->query_start, sizeof(s->query_start), "%s", PQgetvalue(res, i, 9));
        snprintf(s->query, sizeof(s->query), "%s", PQgetvalue(res, i, 10));

        const char *bltxt = PQgetvalue(res, i, 11);
        if (bltxt != NULL && bltxt[0] != '\0') {
            char buffer[256]; snprintf(buffer, sizeof(buffer), "%s", bltxt);
            char *save = NULL; char *token = strtok_r(buffer, ",", &save);
            while (token != NULL && s->blocker_count < PGWA_MAX_BLOCKERS) {
                char *endptr = NULL; long pid = strtol(token, &endptr, 10);
                if (endptr != token && pid > 0) {
                    s->blockers[s->blocker_count] = (pid_t)pid;
                    if (s->blocker_count == 0) s->blocker_pid = (pid_t)pid;
                    s->blocker_count++;
                }
                token = strtok_r(NULL, ",", &save);
            }
        }

        s->xact_age_sec = strtod(PQgetvalue(res, i, 12), NULL);
        s->query_age_sec = strtod(PQgetvalue(res, i, 13), NULL);
        s->leader_pid = (pid_t)strtol(PQgetvalue(res, i, 14), NULL, 10); //leader_pid
    }
    PQclear(res); *session_count = rows; return 0;
}
