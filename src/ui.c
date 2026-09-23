#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include "pgwa.h"
#include "postgres.h"
#include "metrics.h"
#include "ui.h"

static pgwa_session sessions[PGWA_MAX_ROWS];
static pgwa_session filtered_sessions[PGWA_MAX_ROWS];
static int selected_row = 0, top_row = 0, query_scroll = 0;
static char current_sort = 't'; 
static int filter_mode = 0;     
static char filter_user[32] = "";

#define COLOR_PAIR_DEFAULT 1
#define COLOR_PAIR_ACTIVE  2
#define COLOR_PAIR_ABORTED 3
#define COLOR_PAIR_LOCK    4
#define COLOR_PAIR_BLUE    5

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

static void draw_hline(int y, int width) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    mvhline(y, 0, ACS_HLINE, width);
}

static void print_rate_at(int y, int x, double bytes_per_sec) {
    if (bytes_per_sec >= 1024.0 * 1024.0 * 1024.0) 
        mvprintw(y, x, "%6.1fG", bytes_per_sec / (1024.0 * 1024.0 * 1024.0));
    else if (bytes_per_sec >= 1024.0 * 1024.0) 
        mvprintw(y, x, "%6.1fM", bytes_per_sec / (1024.0 * 1024.0));
    else if (bytes_per_sec >= 1024.0) 
        mvprintw(y, x, "%6.1fK", bytes_per_sec / 1024.0);
    else 
        mvprintw(y, x, "%6.0f", bytes_per_sec);
}

static const char *display_wait_type(const pgwa_session *s) {
    if (s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_type;
}

static const char *display_wait_event(const pgwa_session *s) {
    if (s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_event;
}

static int compare_sessions(const void *a, const void *b) {
    const pgwa_session *sa = (const pgwa_session *)a;
    const pgwa_session *sb = (const pgwa_session *)b;
    switch (current_sort) {
        case 'c': return (sb->cpu_ms > sa->cpu_ms) ? 1 : ((sb->cpu_ms < sa->cpu_ms) ? -1 : 0);
        case 'r': return (sb->read_per_sec > sa->read_per_sec) ? 1 : ((sb->read_per_sec < sa->read_per_sec) ? -1 : 0);
        case 'w': return (sb->write_per_sec > sa->write_per_sec) ? 1 : ((sb->write_per_sec < sa->write_per_sec) ? -1 : 0);
        case 'p': return (sa->pid - sb->pid);
        case 't': default: return (sb->runtime_sec > sa->runtime_sec) ? 1 : ((sb->runtime_sec < sa->runtime_sec) ? -1 : 0);
    }
}

static int apply_filter_and_sort(int total_count) {
    int count = 0;
    for (int i = 0; i < total_count; i++) {
        int keep = 1;
        if (filter_mode == 1 && strcmp(sessions[i].state, "active") != 0) keep = 0;
        else if (filter_mode == 2 && sessions[i].blocker_pid <= 0 && strstr(sessions[i].wait_type, "Lock") == NULL) keep = 0;
        else if (filter_mode == 3 && strcasecmp(sessions[i].user, filter_user) != 0) keep = 0;
        if (keep) filtered_sessions[count++] = sessions[i];
    }
    if (count > 0) qsort(filtered_sessions, count, sizeof(pgwa_session), compare_sessions);
    return count;
}
static pgwa_session *find_session(pid_t pid, int session_count) {
    for (int i = 0; i < session_count; i++) if (filtered_sessions[i].pid == pid) return &filtered_sessions[i];
    return NULL;
}

static int session_is_blocker(pid_t pid, int session_count) {
    for (int i = 0; i < session_count; i++)
        for (int j = 0; j < filtered_sessions[i].blocker_count; j++)
            if (filtered_sessions[i].blockers[j] == pid) return 1;
    return 0;
}

static int session_has_blocker(pid_t pid, int session_count) {
    pgwa_session *s = find_session(pid, session_count);
    return (s != NULL && s->blocker_count > 0);
}

static int already_visited(pid_t pid, pid_t *visited, int visited_count) {
    for (int i = 0; i < visited_count; i++) if (visited[i] == pid) return 1;
    return 0;
}

static int draw_tree_node(int y, int max_y, pid_t pid, int depth, int session_count, pid_t *visited, int *visited_count) {
    if (y >= max_y) return y;
    pgwa_session *s = find_session(pid, session_count);
    if (s == NULL || already_visited(pid, visited, *visited_count)) return y;
    if (*visited_count < PGWA_MAX_ROWS) visited[(*visited_count)++] = pid;
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    move(y, 0);
    for (int i = 0; i < depth * 2; i++) addch(' ');
    printw("%-6d %-22.22s", (int)s->pid, s->state);
    if (s->wait_type[0] != '\0') {
        printw("  [%s", s->wait_type);
        if (s->wait_event[0] != '\0') printw(" / %s]", s->wait_event);
        else printw("]");
    }
    y++;
    for (int i = 0; i < session_count; i++) {
        pgwa_session *child = &filtered_sessions[i];
        int is_child = 0;
        for (int j = 0; j < child->blocker_count; j++) if (child->blockers[j] == pid) { is_child = 1; break; }
        if (!is_child || y >= max_y) continue;
        y = draw_tree_node(y, max_y, child->pid, depth + 1, session_count, visited, visited_count);
    }
    return y;
}

static int draw_blocking_tree(int y, int height, int width, int session_count) {
    if (height <= 0) return 0;
    draw_hline(y, width);
    attron(A_BOLD); mvprintw(y + 1, 0, " BLOCKING TREE"); attroff(A_BOLD);
    if (session_count <= 0) { mvprintw(y + 2, 0, " No sessions"); return 3; }
    pid_t visited[PGWA_MAX_ROWS]; int visited_count = 0, tree_y = y + 2, max_y = y + height, printed_tree = 0;
    for (int i = 0; i < session_count; i++) {
        pgwa_session *s = &filtered_sessions[i];
        if (!session_is_blocker(s->pid, session_count) || session_has_blocker(s->pid, session_count) || already_visited(s->pid, visited, visited_count)) continue;
        if (tree_y >= max_y) break;
        tree_y = draw_tree_node(tree_y, max_y, s->pid, 0, session_count, visited, &visited_count);
        printed_tree = 1;
    }
    if (!printed_tree) { mvprintw(y + 2, 0, " No blocking chains"); return 3; }
    return height;
}

static void draw_header(int width, int session_count, int total_count) {
    erase();
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_ACTIVE)); 
    mvprintw(0, 0, " pg_whoisactive"); 
    attroff(A_BOLD);
    mvprintw(0, 18, "PostgreSQL session monitor ");
    attroff(COLOR_PAIR(COLOR_PAIR_ACTIVE));
    
    attron(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("[Sort: "); attroff(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("%c", current_sort); attroff(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE));
    attron(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("]"); attroff(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    
    char label[32]; char value[32];
    if (filter_mode == 0) {
        snprintf(label, sizeof(label), "Sessions: ");
        snprintf(value, sizeof(value), "%d", total_count);
    } else {
        snprintf(label, sizeof(label), "Filtered: ");
        snprintf(value, sizeof(value), "%d/%d", session_count, total_count);
    }
    int total_len = (int)strlen(label) + (int)strlen(value);
    int x = width - total_len - 1; if (x < 60) x = 60;
    
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, x, "%s", label); attroff(COLOR_PAIR(COLOR_PAIR_ACTIVE));
    attron(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%s", value); attroff(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    draw_hline(1, width);
}

static void draw_table_header(int y) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    attron(A_BOLD);
    mvprintw(y, COL_PID,        "PID");  mvprintw(y, COL_USER,       "USER");
    mvprintw(y, COL_DB,         "DB");   mvprintw(y, COL_STATE,      "STATE");
    mvprintw(y, COL_TIME,       "TIME"); mvprintw(y, COL_CPU,        "CPUms");
    mvprintw(y, COL_READ,       "READ/s");mvprintw(y, COL_WRITE,      "WRITE/s");
    mvprintw(y, COL_RSS,        "RSS");  mvprintw(y, COL_BLOCKER,    "BLOCKER");
    mvprintw(y, COL_WAIT_TYPE,  "WAIT_TYPE");mvprintw(y, COL_WAIT_EVENT, "WAIT_EVENT");
    mvprintw(y, COL_HOST,       "HOST");
    attroff(A_BOLD);
}

static void draw_session_row(int y, int row, int width) {
    pgwa_session *s = &filtered_sessions[row];
    int pair = COLOR_PAIR_DEFAULT;
    int is_lock = 0;

    if (strstr(s->state, "aborted") != NULL) pair = COLOR_PAIR_ABORTED;
    else if (strstr(s->wait_type, "Lock") != NULL || s->blocker_pid > 0) { pair = COLOR_PAIR_LOCK; is_lock = 1; }
    else if (strcmp(s->state, "active") == 0) pair = COLOR_PAIR_ACTIVE;

    attrset(COLOR_PAIR(pair));
    if (is_lock) attron(A_BOLD); 
    if (row == selected_row) attron(A_REVERSE);

    mvprintw(y, COL_PID, "%-7d", (int)s->pid);
    mvprintw(y, COL_USER, "%-12.12s", s->user);
    mvprintw(y, COL_DB, "%-10.10s", s->database);
    const char *state = (strcmp(s->state, "idle in transaction (aborted)") == 0) ? "idle in txn (aborted)" : s->state;
    mvprintw(y, COL_STATE, "%-21.21s", state);
    mvprintw(y, COL_TIME, "%7.0fs", s->runtime_sec);
    mvprintw(y, COL_CPU, "%7llu", (unsigned long long)s->cpu_ms);
    print_rate_at(y, COL_READ, s->read_per_sec);
    print_rate_at(y, COL_WRITE, s->write_per_sec);
    mvprintw(y, COL_RSS, "%5lluM", (unsigned long long)(s->rss_kb / 1024));
    if (s->blocker_pid > 0) mvprintw(y, COL_BLOCKER, "%7d", (int)s->blocker_pid);
    else mvprintw(y, COL_BLOCKER, "%7s", "");
    mvprintw(y, COL_WAIT_TYPE, "%-10.10s", display_wait_type(s));
    mvprintw(y, COL_WAIT_EVENT, "%-16.16s", display_wait_event(s));
    mvprintw(y, COL_HOST, "%-32.32s", s->host);

    int used = COL_HOST + 32;
    while (used < width) { mvaddch(y, used, ' '); used++; }
    attroff(A_REVERSE);
    attroff(A_BOLD);
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
}
static void draw_query_panel(int y, int height, int width, int session_count) {
    if (height <= 0) return;
    attroff(A_REVERSE); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    wbkgdset(stdscr, COLOR_PAIR(COLOR_PAIR_DEFAULT) | A_NORMAL);
    for (int r = 0; r < height; r++) {
        move(y + r, 0); for (int c = 0; c < width; c++) mvaddch(y + r, c, ' ');
    }
    draw_hline(y, width);
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_DEFAULT)); mvprintw(y + 1, 0, " QUERY PANEL "); attroff(A_BOLD | COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (session_count <= 0) { mvprintw(y + 2, 0, " No active sessions selected."); return; }

    pgwa_session *s = &filtered_sessions[selected_row];
    move(y + 2, 0);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw(" PID: ");    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%d  ", (int)s->pid);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("User: ");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-16.16s  ", s->user);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("DB: ");     attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-16.16s  ", s->database);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Application: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-28.28s ", s->application);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Host: ");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-32.32s ", s->host);
    move(y + 3, 0);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw(" Started: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    printw("%s  ", s->query_start[0] != '\0' ? (strlen(s->query_start) >= 8 ? s->query_start + strlen(s->query_start) - 8 : s->query_start) : "-");
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Runtime: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    int runtime = (int)s->runtime_sec; if (runtime < 0) runtime = 0;
    printw("%02d:%02d:%02d", runtime / 3600, (runtime % 3600) / 60, runtime % 60);

    int query_y = y + 4, max_query_lines = height - 4; if (max_query_lines <= 0) return;
    const char *p = s->query; int row = 0, col = 1, skipped = 0;
    if (query_scroll > 0) {
        int temp_col = 1;
        while (*p && skipped < query_scroll) {
            char c = *p++; if (c == '\r') continue;
            if (c == '\n') { skipped++; temp_col = 1; continue; }
            if (c == '\t') c = ' ';
            if (temp_col >= width - 1) { skipped++; temp_col = 1; }
            temp_col++;
        }
    }
    int is_comment = 0; move(query_y + row, col);
    while (*p && row < max_query_lines) {
        if (col == 1 || is_comment == 0) { if (*p == '-' && *(p + 1) == '-') is_comment = 1; }
        char c = *p++; if (c == '\r') continue;
        if (c == '\n') {
            is_comment = 0; row++; if (row >= max_query_lines) break;
            move(query_y + row, 1); col = 1; continue;
        }
        if (c == '\t') c = ' ';
        if (col >= width - 1) { row++; if (row >= max_query_lines) break; move(query_y + row, 1); col = 1; }
        if (is_comment) { attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); } 
        else {
            const char *kw[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "DO", "DECLARE", "BEGIN", "END", "$$"};
            int is_keyword = 0;
            size_t num_keywords = sizeof(kw) / sizeof(kw[0]);
            for (size_t k = 0; k < num_keywords; k++) {
                size_t len = strlen(kw[k]); if (strncmp(p - 1, kw[k], len) == 0) { is_keyword = 1; break; }
            }
            if (is_keyword || c == '$') attrset(COLOR_PAIR(COLOR_PAIR_LOCK) | A_BOLD); 
            else attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); 
        }
        mvaddch(query_y + row, col, c); col++;
    }
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
}

static void draw_footer(int y, int width) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); draw_hline(y, width); move(y + 1, 0);
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw(" Up/Down");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Select Row  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("Sort:[t/c/r/w/p]"); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("f");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Filter  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("k");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Kill/Cancel  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("PgUp/PgDn");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Scroll Query  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("q");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Quit");
}
static void normalize_selection(int session_count, int visible_rows) {
    if (session_count <= 0) { selected_row = 0; top_row = 0; return; }
    if (selected_row >= session_count) selected_row = session_count - 1;
    if (selected_row < 0) selected_row = 0;
    if (top_row > selected_row) top_row = selected_row;
    if (selected_row >= top_row + visible_rows) top_row = selected_row - visible_rows + 1;
    if (top_row < 0) top_row = 0;
}

static void handle_filter_menu(int width, int height) {
    (void)width; timeout(-1); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
    mvprintw(height - 1, 0, "Filters: 0:All  1:Active  2:Locks  3:User. Choose [0-3]: "); refresh();
    echo(); curs_set(1); int ch = getch();
    if (ch >= '0' && ch <= '3') {
        filter_mode = ch - '0';
        if (filter_mode == 3) {
            move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
            mvprintw(height - 1, 0, "Enter username: "); refresh();
            mvgetnstr(height - 1, 16, filter_user, sizeof(filter_user) - 1);
        }
    }
    noecho(); curs_set(0); selected_row = 0; top_row = 0; timeout(1000); 
}

static void handle_kill_menu(int width, int height, pgwa_pg_connection *pg) {
    (void)width;
    if (filtered_sessions[selected_row].pid <= 0) return;
    pid_t target_pid = filtered_sessions[selected_row].pid;
    timeout(-1); 
    attrset(COLOR_PAIR(COLOR_PAIR_ABORTED)); attron(A_BOLD);
    move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
    mvprintw(height - 1, 0, "TARGET PID %d. Actions: c:Cancel backend, k:Kill/Terminate. Choice: ", (int)target_pid); attroff(A_BOLD);
    refresh();
    
    int action = getch();
    char query[256];
    
    if (action == 'c' || action == 'C') {
        move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
        snprintf(query, sizeof(query), "SELECT pg_cancel_backend(%d);", (int)target_pid);
        postgres_execute(pg, query);
        
        mvprintw(height - 1, 0, "Sent pg_cancel_backend(%d). Press any key to continue... ", (int)target_pid); 
        refresh(); getch();
    } else if (action == 'k' || action == 'K') {
        move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
        mvprintw(height - 1, 0, "ARE YOU SURE to terminate PID %d? (y/n): ", (int)target_pid);
        refresh(); int confirm = getch();
        if (confirm == 'y' || confirm == 'Y') {
            move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
            snprintf(query, sizeof(query), "SELECT pg_terminate_backend(%d);", (int)target_pid);
            postgres_execute(pg, query);
            
            mvprintw(height - 1, 0, "Sent pg_terminate_backend(%d). Press any key to continue... ", (int)target_pid); 
            refresh(); getch();
        }
    }
    timeout(1000); 
}

static int handle_key(int key, int session_count, int width, int height, pgwa_pg_connection *pg) {
    int old_selected = selected_row;
    switch (key) {
        case KEY_UP: if (selected_row > 0) selected_row--; break;
        case KEY_DOWN: if (selected_row < session_count - 1) selected_row++; break;
        case KEY_PPAGE: if (query_scroll >= 5) query_scroll -= 5; else query_scroll = 0; break;
        case KEY_NPAGE: query_scroll += 5; break;
        case KEY_HOME: query_scroll = 0; break;
        case 't': case 'c': case 'r': case 'w': case 'p': current_sort = key; break;
        case 'f': case 'F': handle_filter_menu(width, height); break;
        case 'k': case 'K': handle_kill_menu(width, height, pg); break;
        case 'q': case 'Q': return 1;
    }
    if (old_selected != selected_row) query_scroll = 0;
    return 0;
}

void ui_run(void) {
    pgwa_pg_connection pg = {0}; int total_count = 0;
    if (postgres_connect(&pg) != 0) { fprintf(stderr, "Cannot connect to PostgreSQL\n"); return; }
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(1000);
    if (has_colors()) {
        start_color();
        init_pair(COLOR_PAIR_DEFAULT, COLOR_WHITE,   COLOR_BLACK);
        init_pair(COLOR_PAIR_ACTIVE,  COLOR_GREEN,   COLOR_BLACK);
        init_pair(COLOR_PAIR_ABORTED, COLOR_RED,     COLOR_BLACK);
        init_pair(COLOR_PAIR_LOCK,    COLOR_YELLOW,  COLOR_BLACK);
        init_pair(COLOR_PAIR_BLUE,    COLOR_CYAN,    COLOR_BLACK);
    }
    while (1) {
        int height, width; getmaxyx(stdscr, height, width);
        if (height < 18 || width < 120) {
            erase(); attron(A_BOLD); mvprintw(0, 0, "pg_whoisactive"); attroff(A_BOLD);
            mvprintw(2, 0, "Terminal is too small. Minimum size: 120x18"); refresh();
            int key = getch(); if (key == 'q' || key == 'Q') break;
            continue;
        }
        if (postgres_read_activity(&pg, sessions, PGWA_MAX_ROWS, &total_count) != 0) break;
        metrics_update(sessions, total_count);
        int session_count = apply_filter_and_sort(total_count);
        int footer_y = height - 2, query_height = 10, query_y = footer_y - query_height;
        int tree_y = query_y - 8, table_y = 2, table_height = tree_y - table_y;

        erase();
        draw_header(width, session_count, total_count);
        draw_table_header(table_y);
        int visible_rows = table_height - 1; if (visible_rows < 0) visible_rows = 0;
        normalize_selection(session_count, visible_rows);
        for (int i = 0; i < visible_rows; i++) {
            int idx = top_row + i; if (idx >= session_count) break;
            draw_session_row(table_y + 1 + i, idx, width);
        }
        draw_blocking_tree(tree_y, 8, width, session_count);
        draw_query_panel(query_y, query_height, width, session_count);
        draw_footer(footer_y, width);
        refresh();
        int key = getch();
        if (key != ERR && handle_key(key, session_count, width, height, &pg)) break;
    }
    endwin(); postgres_disconnect(&pg);
}
