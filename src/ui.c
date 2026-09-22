#include <stdio.h>
#include <string.h>

#include <ncurses.h>

#include "pgwa.h"
#include "postgres.h"
#include "metrics.h"
#include "ui.h"

static pgwa_session sessions[PGWA_MAX_ROWS];

static int selected_row = 0;


/*
 * Table columns.
 */
#define COL_PID        0
#define COL_USER       8
#define COL_DB         21
#define COL_STATE      32
#define COL_TIME       54
#define COL_CPU        62
#define COL_READ       70
#define COL_WRITE      79
#define COL_RSS        89
#define COL_BLOCKER    98
#define COL_WAIT_TYPE  106
#define COL_WAIT_EVENT 117
#define COL_HOST       135


static void draw_hline(int y,
                       int width)
{
    mvhline(y,
            0,
            ACS_HLINE,
            width);
}


static void print_rate_at(int y,
                          int x,
                          double bytes_per_sec)
{
    if (bytes_per_sec >=
        1024.0 * 1024.0 * 1024.0)
    {
        mvprintw(y,
                 x,
                 "%6.1fG",
                 bytes_per_sec /
                 (1024.0 * 1024.0 * 1024.0));
    }
    else if (bytes_per_sec >=
             1024.0 * 1024.0)
    {
        mvprintw(y,
                 x,
                 "%6.1fM",
                 bytes_per_sec /
                 (1024.0 * 1024.0));
    }
    else if (bytes_per_sec >=
             1024.0)
    {
        mvprintw(y,
                 x,
                 "%6.1fK",
                 bytes_per_sec /
                 1024.0);
    }
    else
    {
        mvprintw(y,
                 x,
                 "%6.0f",
                 bytes_per_sec);
    }
}


static const char *display_wait_type(
    const pgwa_session *s)
{
    if (s->state[0] == '\0')
        return "";

    if (strcmp(s->state,
               "idle") == 0)
    {
        return "";
    }

    return s->wait_type;
}


static const char *display_wait_event(
    const pgwa_session *s)
{
    if (s->state[0] == '\0')
        return "";

    if (strcmp(s->state,
               "idle") == 0)
    {
        return "";
    }

    return s->wait_event;
}


/*
 * --------------------------------------------------------------------------
 * Blocking tree
 * --------------------------------------------------------------------------
 */

static pgwa_session *find_session(
    pid_t pid,
    int session_count)
{
    for (int i = 0;
         i < session_count;
         i++)
    {
        if (sessions[i].pid == pid)
            return &sessions[i];
    }

    return NULL;
}


static int session_is_blocker(
    pid_t pid,
    int session_count)
{
    for (int i = 0;
         i < session_count;
         i++)
    {
        for (int j = 0;
             j < sessions[i].blocker_count;
             j++)
        {
            if (sessions[i].blockers[j] == pid)
                return 1;
        }
    }

    return 0;
}


static int session_has_blocker(
    pid_t pid,
    int session_count)
{
    pgwa_session *s =
        find_session(pid,
                     session_count);

    if (s == NULL)
        return 0;

    return s->blocker_count > 0;
}


static int already_visited(
    pid_t pid,
    pid_t *visited,
    int visited_count)
{
    for (int i = 0;
         i < visited_count;
         i++)
    {
        if (visited[i] == pid)
            return 1;
    }

    return 0;
}


static int draw_tree_node(
    int y,
    int max_y,
    pid_t pid,
    int depth,
    int session_count,
    pid_t *visited,
    int *visited_count)
{
    if (y >= max_y)
        return y;

    pgwa_session *s =
        find_session(pid,
                     session_count);

    if (s == NULL)
        return y;

    if (already_visited(pid,
                        visited,
                        *visited_count))
    {
        return y;
    }

    if (*visited_count < PGWA_MAX_ROWS)
    {
        visited[*visited_count] = pid;
        (*visited_count)++;
    }

    int indent =
        depth * 2;

    move(y,
         0);

    for (int i = 0;
         i < indent;
         i++)
    {
        addch(' ');
    }

    printw("%-6d %-22.22s",
           (int)s->pid,
           s->state);

    if (s->wait_type[0] != '\0')
    {
        printw("  [%s",
               s->wait_type);

        if (s->wait_event[0] != '\0')
        {
            printw(" / %s]",
                   s->wait_event);
        }
        else
        {
            printw("]");
        }
    }

    y++;

    for (int i = 0;
         i < session_count;
         i++)
    {
        pgwa_session *child =
            &sessions[i];

        int is_child = 0;

        for (int j = 0;
             j < child->blocker_count;
             j++)
        {
            if (child->blockers[j] == pid)
            {
                is_child = 1;
                break;
            }
        }

        if (!is_child)
            continue;

        if (y >= max_y)
            break;

        y =
            draw_tree_node(
                y,
                max_y,
                child->pid,
                depth + 1,
                session_count,
                visited,
                visited_count);
    }

    return y;
}


static int draw_blocking_tree(
    int y,
    int height,
    int width,
    int session_count)
{
    if (height <= 0)
        return 0;

    draw_hline(y,
               width);

    attron(A_BOLD);

    mvprintw(y + 1,
             0,
             " BLOCKING TREE");

    attroff(A_BOLD);

    if (session_count <= 0)
    {
        mvprintw(y + 2,
                 0,
                 " No sessions");

        return 3;
    }

    pid_t visited[PGWA_MAX_ROWS];

    int visited_count = 0;

    int tree_y =
        y + 2;

    int max_y =
        y + height;

    int printed_tree = 0;

    for (int i = 0;
         i < session_count;
         i++)
    {
        pgwa_session *s =
            &sessions[i];

        if (!session_is_blocker(
                s->pid,
                session_count))
        {
            continue;
        }

        if (session_has_blocker(
                s->pid,
                session_count))
        {
            continue;
        }

        if (already_visited(
                s->pid,
                visited,
                visited_count))
        {
            continue;
        }

        if (tree_y >= max_y)
            break;

        tree_y =
            draw_tree_node(
                tree_y,
                max_y,
                s->pid,
                0,
                session_count,
                visited,
                &visited_count);

        printed_tree = 1;
    }

    if (!printed_tree)
    {
        for (int i = 0;
             i < session_count;
             i++)
        {
            pgwa_session *s =
                &sessions[i];

            if (!session_is_blocker(
                    s->pid,
                    session_count))
            {
                continue;
            }

            if (already_visited(
                    s->pid,
                    visited,
                    visited_count))
            {
                continue;
            }

            if (tree_y >= max_y)
                break;

            tree_y =
                draw_tree_node(
                    tree_y,
                    max_y,
                    s->pid,
                    0,
                    session_count,
                    visited,
                    &visited_count);

            printed_tree = 1;
        }
    }

    if (!printed_tree)
    {
        mvprintw(y + 2,
                 0,
                 " No blocking chains");

        return 3;
    }

    return height;
}


/*
 * --------------------------------------------------------------------------
 * Header
 * --------------------------------------------------------------------------
 */

static void draw_header(
    int width,
    int session_count)
{
    erase();

    attron(A_BOLD);

    mvprintw(0,
             0,
             " pg_whoisactive");

    attroff(A_BOLD);

    mvprintw(0,
             18,
             "PostgreSQL session monitor");

    char text[32];

    snprintf(text,
             sizeof(text),
             "Sessions: %d",
             session_count);

    int x =
        width -
        (int)strlen(text) -
        1;

    if (x < 40)
        x = 40;

    mvprintw(0,
             x,
             "%s",
             text);

    draw_hline(1,
               width);
}


/*
 * --------------------------------------------------------------------------
 * Session table
 * --------------------------------------------------------------------------
 */

static void draw_table_header(int y)
{
    attron(A_BOLD);

    mvprintw(y, COL_PID,        "PID");
    mvprintw(y, COL_USER,       "USER");
    mvprintw(y, COL_DB,         "DB");
    mvprintw(y, COL_STATE,      "STATE");
    mvprintw(y, COL_TIME,       "TIME");
    mvprintw(y, COL_CPU,        "CPUms");
    mvprintw(y, COL_READ,       "READ/s");
    mvprintw(y, COL_WRITE,      "WRITE/s");
    mvprintw(y, COL_RSS,        "RSS");
    mvprintw(y, COL_BLOCKER,    "BLOCKER");
    mvprintw(y, COL_WAIT_TYPE,  "WAIT_TYPE");
    mvprintw(y, COL_WAIT_EVENT, "WAIT_EVENT");
    mvprintw(y, COL_HOST,       "HOST");

    attroff(A_BOLD);
}


static void draw_session_row(
    int y,
    int row,
    int width)
{
    pgwa_session *s =
        &sessions[row];

    if (row == selected_row)
        attron(A_REVERSE);

    const char *wait_type =
        display_wait_type(s);

    const char *wait_event =
        display_wait_event(s);

    mvprintw(y,
             COL_PID,
             "%-7d",
             (int)s->pid);

    mvprintw(y,
             COL_USER,
             "%-12.12s",
             s->user);

    mvprintw(y,
             COL_DB,
             "%-10.10s",
             s->database);

    mvprintw(y,
             COL_STATE,
             "%-21.21s",
             s->state);

    mvprintw(y,
             COL_TIME,
             "%7.0fs",
             s->runtime_sec);

    mvprintw(y,
             COL_CPU,
             "%7llu",
             (unsigned long long)s->cpu_ms);

    print_rate_at(y,
                  COL_READ,
                  s->read_per_sec);

    print_rate_at(y,
                  COL_WRITE,
                  s->write_per_sec);

    mvprintw(y,
             COL_RSS,
             "%5lluM",
             (unsigned long long)
             (s->rss_kb / 1024));

    if (s->blocker_pid > 0)
    {
        mvprintw(y,
                 COL_BLOCKER,
                 "%7d",
                 (int)s->blocker_pid);
    }
    else
    {
        mvprintw(y,
                 COL_BLOCKER,
                 "%7s",
                 "");
    }

    mvprintw(y,
             COL_WAIT_TYPE,
             "%-10.10s",
             wait_type);

    mvprintw(y,
             COL_WAIT_EVENT,
             "%-16.16s",
             wait_event);

    mvprintw(y,
             COL_HOST,
             "%-32.32s",
             s->host);

    int used =
        COL_HOST + 32;

    while (used < width)
    {
        mvaddch(y,
                used,
                ' ');

        used++;
    }

    if (row == selected_row)
        attroff(A_REVERSE);
}


/*
 * --------------------------------------------------------------------------
 * Query panel
 * --------------------------------------------------------------------------
 */

static void draw_query_panel(
    int y,
    int height,
    int width,
    int session_count)
{
    if (height <= 0)
        return;

    draw_hline(y,
               width);

    attron(A_BOLD);

    mvprintw(y + 1,
             0,
             " QUERY");

    attroff(A_BOLD);

    if (session_count <= 0)
    {
        mvprintw(y + 2,
                 0,
                 " No sessions");

        return;
    }

    pgwa_session *s =
        &sessions[selected_row];

    /*
     * First context line:
     * PID / User / DB / Host
     */
    mvprintw(y + 2,
             0,
             " PID: %d  User: %-16.16s  DB: %-16.16s  Host: %-32.32s",
             (int)s->pid,
             s->user,
             s->database,
             s->host);

    /*
     * Second context line:
     * Application / Started / Runtime
     */
    mvprintw(y + 3,
             0,
             " Application: %-28.28s  Started: %s  Runtime: ",
             s->application,
             s->query_start[0] != '\0'
                 ? (strlen(s->query_start) >= 8
                        ? s->query_start +
                          strlen(s->query_start) - 8
                        : s->query_start)
                 : "-");

    /*
     * Runtime in HH:MM:SS.
     */
    int runtime =
        (int)s->runtime_sec;

    if (runtime < 0)
        runtime = 0;

    int hours =
        runtime / 3600;

    int minutes =
        (runtime % 3600) / 60;

    int seconds =
        runtime % 60;

    printw("%02d:%02d:%02d",
           hours,
           minutes,
           seconds);

    int query_y =
        y + 4;

    int max_query_lines =
        height - 4;

    if (max_query_lines <= 0)
        return;

    const char *p =
        s->query;

    for (int line = 0;
         line < max_query_lines && *p;
         line++)
    {
        mvaddch(query_y + line,
                0,
                ' ');

        int col = 1;

        while (*p &&
               *p != '\n' &&
               *p != '\r' &&
               col < width)
        {
            char c = *p++;

            if (c == '\t')
                c = ' ';

            mvaddch(query_y + line,
                    col,
                    c);

            col++;
        }

        if (*p == '\n' ||
            *p == '\r')
        {
            char newline_char =
                *p;

            p++;

            if (newline_char == '\r' &&
                *p == '\n')
            {
                p++;
            }
        }

        while (col < width)
        {
            mvaddch(query_y + line,
                    col,
                    ' ');

            col++;
        }
    }
}


/*
 * --------------------------------------------------------------------------
 * Footer
 * --------------------------------------------------------------------------
 */

static void draw_footer(
    int y,
    int width)
{
    draw_hline(y,
               width);

    attron(A_BOLD);

    mvprintw(y + 1,
             0,
             " Up/Down Select   q Quit");

    attroff(A_BOLD);

    mvprintw(y + 1,
             30,
             "Refresh: 1s");

    if (width > 0)
        move(y + 1,
             width - 1);
}


/*
 * --------------------------------------------------------------------------
 * Keyboard
 * --------------------------------------------------------------------------
 */

static void normalize_selection(
    int session_count)
{
    if (session_count <= 0)
    {
        selected_row = 0;
        return;
    }

    if (selected_row >= session_count)
        selected_row =
            session_count - 1;

    if (selected_row < 0)
        selected_row = 0;
}


static int handle_key(
    int key,
    int session_count)
{
    switch (key)
    {
        case KEY_UP:

            if (selected_row > 0)
                selected_row--;

            break;

        case KEY_DOWN:

            if (selected_row <
                session_count - 1)
            {
                selected_row++;
            }

            break;

        case 'q':
        case 'Q':

            return 1;

        default:

            break;
    }

    return 0;
}


/*
 * --------------------------------------------------------------------------
 * Main UI
 * --------------------------------------------------------------------------
 */

void ui_run(void)
{
    pgwa_pg_connection pg = {0};

    int session_count = 0;

    if (postgres_connect(&pg) != 0)
    {
        fprintf(stderr,
                "Cannot connect to PostgreSQL\n");

        return;
    }

    initscr();

    cbreak();
    noecho();

    keypad(stdscr,
           TRUE);

    curs_set(0);

    timeout(1000);

    if (has_colors())
    {
        start_color();
        use_default_colors();
    }

    while (1)
    {
        int height;
        int width;

        getmaxyx(stdscr,
                 height,
                 width);

        /*
         * HOST is now at column 135.
         */
        if (height < 18 ||
            width < 167)
        {
            erase();

            attron(A_BOLD);

            mvprintw(0,
                     0,
                     "pg_whoisactive");

            attroff(A_BOLD);

            mvprintw(2,
                     0,
                     "Terminal is too small.");

            mvprintw(3,
                     0,
                     "Minimum size: 167x18");

            mvprintw(5,
                     0,
                     "Current size: %dx%d",
                     width,
                     height);

            mvprintw(7,
                     0,
                     "Resize the terminal or press q to quit.");

            refresh();

            int key =
                getch();

            if (key == 'q' ||
                key == 'Q')
            {
                break;
            }

            continue;
        }

        if (postgres_read_activity(
                &pg,
                sessions,
                PGWA_MAX_ROWS,
                &session_count) != 0)
        {
            break;
        }

        metrics_update(
            sessions,
            session_count);

        normalize_selection(
            session_count);

        int footer_y =
            height - 2;

        /*
         * Query panel is now slightly taller
         * because it has two context lines.
         */
        int query_height = 6;

        int query_y =
            footer_y -
            query_height;

        int tree_height = 8;

        int tree_y =
            query_y -
            tree_height;

        int table_y = 2;

        int table_height =
            tree_y -
            table_y;

        erase();

        draw_header(width,
                    session_count);

        draw_table_header(table_y);

        int visible_rows =
            table_height - 1;

        if (visible_rows < 0)
            visible_rows = 0;

        for (int i = 0;
             i < session_count &&
             i < visible_rows;
             i++)
        {
            draw_session_row(
                table_y + 1 + i,
                i,
                width);
        }

        if (session_count > visible_rows &&
            visible_rows > 0)
        {
            mvprintw(
                table_y + visible_rows,
                0,
                "... %d more sessions ...",
                session_count -
                visible_rows);
        }

        draw_blocking_tree(
            tree_y,
            tree_height,
            width,
            session_count);

        draw_query_panel(
            query_y,
            query_height,
            width,
            session_count);

        draw_footer(
            footer_y,
            width);

        refresh();

        int key =
            getch();

        if (key != ERR)
        {
            if (handle_key(
                    key,
                    session_count))
            {
                break;
            }
        }
    }

    endwin();

    postgres_disconnect(&pg);
}