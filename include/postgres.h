#ifndef POSTGRES_H
#define POSTGRES_H

#include "pgwa.h"

typedef struct
{
    void *conn;
} pgwa_pg_connection;

int postgres_connect(pgwa_pg_connection *pg);

void postgres_disconnect(pgwa_pg_connection *pg);

int postgres_read_activity(pgwa_pg_connection *pg,
                           pgwa_session *sessions,
                           int max_sessions,
                           int *session_count);

#endif