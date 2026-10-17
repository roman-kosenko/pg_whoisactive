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

#ifndef POSTGRES_H
#define POSTGRES_H

#include "pgwa.h"

typedef struct
{
    void *conn;
    uint64_t total_xacts;
    int av_active;       
    int av_max;          
} pgwa_pg_connection;

int postgres_connect(pgwa_pg_connection *pg, const char *port);
void postgres_disconnect(pgwa_pg_connection *pg);
int postgres_read_activity(pgwa_pg_connection *pg,
                           pgwa_session *sessions,
                           int max_sessions,
                           int *session_count);
int postgres_execute(pgwa_pg_connection *pg, const char *sql);
#endif
