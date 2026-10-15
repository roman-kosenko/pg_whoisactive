#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include <locale.h>  
#include <wchar.h>   
#include <libpq-fe.h>  

#include "pgwa.h"
#include "postgres.h"
#include "metrics.h"
#include "ui.h"

int wcwidth(wchar_t wc);

typedef enum { ROW_CLIENT_SESSION, ROW_PARALLEL_WORKER } row_type_t;

typedef struct {
    row_type_t type;
    int session_idx;      
    pid_t pid;
    pid_t leader_pid;
    pgwa_session *ptr;    
} display_row_t;

static display_row_t display_rows[PGWA_MAX_ROWS * 2]; 
static int display_rows_count = 0;

static pgwa_session sessions[PGWA_MAX_ROWS];
static pgwa_session filtered_sessions[PGWA_MAX_ROWS];
static int filtered_sessions_count = 0;

static int selected_row = 0, top_row = 0, query_scroll = 0;
static char current_sort = 't'; 
static int filter_mode = 0;     
static char filter_user[32] = "";
static int paused = 0;

#define COLOR_PAIR_DEFAULT  1
#define COLOR_PAIR_ACTIVE   2
#define COLOR_PAIR_ABORTED  3
#define COLOR_PAIR_LOCK     4
#define COLOR_PAIR_BLUE     5
#define COLOR_PAIR_SELECTED 6
#define COLOR_PAIR_WORKER   7

#define COL_PID        0
#define COL_USER       8
#define COL_DB         21
#define COL_STATE      32
#define COL_TIME       54
#define COL_CPU        64   
#define COL_CPUMS      71   
#define COL_READ       80   
#define COL_WRITE      89   
#define COL_RSS        99   
#define COL_DRSS       108  
#define COL_BLOCKER    117  
#define COL_WAIT_TYPE  126  
#define COL_WAIT_EVENT 137  
#define COL_HOST       155  
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

static void print_delta_rss_at(int y, int x, int64_t delta_kb) {
    if (delta_kb == 0) {
        mvprintw(y, x, "%7s", "0M");
        return;
    }
    char sign = (delta_kb > 0) ? '+' : '-';
    uint64_t abs_kb = (delta_kb > 0) ? delta_kb : -delta_kb;
    double bytes = (double)abs_kb * 1024.0;

    if (bytes >= 1024.0 * 1024.0 * 1024.0)
        mvprintw(y, x, "%c%5.1fG", sign, bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024.0 * 1024.0)
        mvprintw(y, x, "%c%5.1fM", sign, bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024.0)
        mvprintw(y, x, "%c%5.0fK", sign, bytes / 1024.0);
    else
        mvprintw(y, x, "%c%5.0fB", sign, bytes);
}

static void format_age_styled(char *buf, size_t max_len, double seconds_val) {
    int64_t total_sec = (int64_t)seconds_val;
    if (total_sec <= 0) {
        snprintf(buf, max_len, "0s");
        return;
    }
    int64_t days = total_sec / 86400;
    int64_t hours = (total_sec % 86400) / 3600;
    int64_t minutes = (total_sec % 3600) / 60;
    int64_t secs = total_sec % 60;

    if (days > 0)
        snprintf(buf, max_len, "%ldd%ldh", days, hours);
    else if (hours > 0)
        snprintf(buf, max_len, "%ldh%ldm", hours, minutes);
    else if (minutes > 0)
        snprintf(buf, max_len, "%ldm%lds", minutes, secs);
    else
        snprintf(buf, max_len, "%lds", secs);
}

static const char *display_wait_type(const pgwa_session *s) {
    if (s == NULL || s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_type;
}

static const char *display_wait_event(const pgwa_session *s) {
    if (s == NULL || s->state[0] == '\0' || strcmp(s->state, "idle") == 0) return "";
    return s->wait_event;
}
static int compare_sessions(const void *a, const void *b) {
    const pgwa_session *sa = (const pgwa_session *)a;
    const pgwa_session *sb = (const pgwa_session *)b;
    switch (current_sort) {
        case 'c': 
            if (sb->cpu_pct != sa->cpu_pct) return (sb->cpu_pct > sa->cpu_pct) ? 1 : -1;
            return (sb->cpu_ms > sa->cpu_ms) ? 1 : ((sb->cpu_ms < sa->cpu_ms) ? -1 : 0);
        case 'm': 
            if (sb->rss_delta_kb != sa->rss_delta_kb) return (sb->rss_delta_kb > sa->rss_delta_kb) ? 1 : -1;
            return (sb->rss_kb > sa->rss_kb) ? 1 : ((sb->rss_kb < sa->rss_kb) ? -1 : 0);
        case 'r': return (sb->read_per_sec > sa->read_per_sec) ? 1 : ((sb->read_per_sec < sa->read_per_sec) ? -1 : 0);
        case 'w': return (sb->write_per_sec > sa->write_per_sec) ? 1 : ((sb->write_per_sec < sa->write_per_sec) ? -1 : 0);
        case 'p': return (sa->pid - sb->pid);
        case 't': default: return (sb->runtime_sec > sa->runtime_sec) ? 1 : ((sb->runtime_sec < sa->runtime_sec) ? -1 : 0);
    }
}

static int apply_filter_and_sort(int total_count) {
    filtered_sessions_count = 0;
    for (int i = 0; i < total_count; i++) {
        if (sessions[i].leader_pid != 0) continue; 

        int keep = 1;
        if (filter_mode == 1 && strcmp(sessions[i].state, "active") != 0) keep = 0;
        else if (filter_mode == 2 && sessions[i].blocker_pid <= 0 && strstr(sessions[i].wait_type, "Lock") == NULL) keep = 0;
        else if (filter_mode == 3 && strcasecmp(sessions[i].user, filter_user) != 0) keep = 0;
        
        if (keep) filtered_sessions[filtered_sessions_count++] = sessions[i];
    }
    if (filtered_sessions_count > 0) {
        qsort(filtered_sessions, filtered_sessions_count, sizeof(pgwa_session), compare_sessions);
    }
    return filtered_sessions_count;
}

static void build_display_rows(int total_count) {
    display_rows_count = 0;
    for (int i = 0; i < filtered_sessions_count; i++) {
        if (display_rows_count >= PGWA_MAX_ROWS * 2) break;
        
        display_rows[display_rows_count].type = ROW_CLIENT_SESSION;
        display_rows[display_rows_count].session_idx = i;
        display_rows[display_rows_count].pid = filtered_sessions[i].pid;
        display_rows[display_rows_count].leader_pid = 0;
        display_rows[display_rows_count].ptr = &filtered_sessions[i];
        display_rows_count++;

        for (int j = 0; j < total_count; j++) {
            if (sessions[j].leader_pid == filtered_sessions[i].pid) {
                if (display_rows_count >= PGWA_MAX_ROWS * 2) break;
                
                display_rows[display_rows_count].type = ROW_PARALLEL_WORKER;
                display_rows[display_rows_count].session_idx = i; 
                display_rows[display_rows_count].pid = sessions[j].pid;
                display_rows[display_rows_count].leader_pid = sessions[j].leader_pid;
                display_rows[display_rows_count].ptr = &sessions[j];
                display_rows_count++;
            }
        }
    }
}
static pgwa_session *find_session(pid_t pid) {
    for (int i = 0; i < filtered_sessions_count; i++) {
        if (filtered_sessions[i].pid == pid) return &filtered_sessions[i];
    }
    return NULL;
}

static int session_is_blocker(pid_t pid) {
    for (int i = 0; i < filtered_sessions_count; i++) {
        for (int j = 0; j < filtered_sessions[i].blocker_count; j++) {
            if (filtered_sessions[i].blockers[j] == pid) return 1;
        }
    }
    return 0;
}

static int session_has_blocker(pid_t pid) {
    pgwa_session *s = find_session(pid);
    return (s != NULL && s->blocker_count > 0);
}

static int already_visited(pid_t pid, pid_t *visited, int visited_count) {
    for (int i = 0; i < visited_count; i++) if (visited[i] == pid) return 1;
    return 0;
}

static int draw_tree_node(int y, int max_y, pid_t pid, int depth, pid_t *visited, int *visited_count) {
    if (y >= max_y) return y;
    pgwa_session *s = find_session(pid);
    if (s == NULL || already_visited(pid, visited, *visited_count)) return y;
    if (*visited_count < PGWA_MAX_ROWS) visited[(*visited_count)++] = pid;

    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    move(y, 0);

    /* Отступ и префикс как у воркеров */
    for (int i = 0; i < depth; i++) printw("  ");
    if (depth > 0) {
        printw("└─");
    }
    printw("%-6d %-16.16s", (int)s->pid, s->state);
    if (s->wait_type[0] != '\0') {
        printw("  [%s", s->wait_type);
        if (s->wait_event[0] != '\0') printw(" / %s]", s->wait_event);
        else printw("]");
    }
    y++;
    for (int i = 0; i < filtered_sessions_count; i++) {
        pgwa_session *child = &filtered_sessions[i];
        int is_child = 0;
        for (int j = 0; j < child->blocker_count; j++) if (child->blockers[j] == pid) { is_child = 1; break; }
        if (!is_child || y >= max_y) continue;
        y = draw_tree_node(y, max_y, child->pid, depth + 1, visited, visited_count);
    }
    return y;
}

static int draw_blocking_tree(int y, int height, int width) {
    if (height <= 0) return 0;
    draw_hline(y, width);
    attron(A_BOLD); mvprintw(y + 1, 0, " BLOCKING TREE"); attroff(A_BOLD);
    if (filtered_sessions_count <= 0) { mvprintw(y + 2, 0, " No sessions"); return 3; }
    pid_t visited[PGWA_MAX_ROWS]; int visited_count = 0, tree_y = y + 2, max_y = y + height, printed_tree = 0;
    for (int i = 0; i < filtered_sessions_count; i++) {
        pgwa_session *s = &filtered_sessions[i];
        if (!session_is_blocker(s->pid) || session_has_blocker(s->pid) || already_visited(s->pid, visited, visited_count)) continue;
        if (tree_y >= max_y) break;
        tree_y = draw_tree_node(tree_y, max_y, s->pid, 0, visited, &visited_count);
        printed_tree = 1;
    }
    if (!printed_tree) { mvprintw(y + 2, 0, " No blocking chains"); return 3; }
    return height;
}
static void draw_header(int width, int session_count, int total_count, pgwa_pg_connection *pg) {
    erase();
    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); 
    mvprintw(0, 0, " ⚡pg_whoisactive "); 
    
    if (pg != NULL && pg->conn != NULL) {
        char *p_port = PQport((PGconn *)pg->conn);
        char *p_db = PQdb((PGconn *)pg->conn);
        char *p_user = PQuser((PGconn *)pg->conn);
        int v_raw = PQserverVersion((PGconn *)pg->conn);
        int v_maj = v_raw / 10000;
        int v_min = (v_raw % 10000) / 100;
        
        attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
        printw("[%s@%s:%s (v%d.%d)] ", p_user ? p_user : "-", p_db ? p_db : "-", p_port ? p_port : "-", v_maj, v_min);
    }

    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("[Sort: "); 
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("%c", current_sort); 
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("] "); 

    int cur_y, cur_x;
    getyx(stdscr, cur_y, cur_x);
    (void)cur_y; 
    
    if (cur_x < 55) cur_x = 55;

    metrics_info g_metrics = metrics_get_global();
    char buf_pct[16];

    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, cur_x, "Host CPU ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); snprintf(buf_pct, sizeof(buf_pct), "%.0f%%", g_metrics.host_cpu_pct);
    printw("%-5s", buf_pct); cur_x += 15;

    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, cur_x, "PG CPU ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); snprintf(buf_pct, sizeof(buf_pct), "%.0f%%", g_metrics.pg_cpu_pct);
    printw("%-5s", buf_pct); cur_x += 13;

    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, cur_x, "CPUs ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-4d", g_metrics.cpu_cores); cur_x += 10;

    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, cur_x, "TPS: ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-6.0f", metrics_get_tps()); cur_x += 12;

    int av_act_val = (pg != NULL) ? pg->av_active : 0;
    int av_max_val = (pg != NULL && pg->av_max > 0) ? pg->av_max : 3;
    int av_color_pair = COLOR_PAIR_ACTIVE; 
    double av_usage_pct = ((double)av_act_val / (double)av_max_val) * 100.0;
    if (av_usage_pct >= 75.0) av_color_pair = COLOR_PAIR_ABORTED; 
    else if (av_usage_pct >= 50.0) av_color_pair = COLOR_PAIR_LOCK;    

    attrset(COLOR_PAIR(av_color_pair)); mvprintw(0, cur_x, "AV ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); char str_av[32];
    snprintf(str_av, sizeof(str_av), "%d/%d", av_act_val, av_max_val);
    printw("%-7s", str_av); cur_x += 11;

    attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE)); mvprintw(0, cur_x, (filter_mode == 0) ? "Sessions: " : "Filtered: ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (filter_mode == 0) printw("%-6d", total_count);
    else { char str_sess[32]; snprintf(str_sess, sizeof(str_sess), "%d/%d", session_count, total_count); printw("%-7s", str_sess); }

    draw_hline(1, width);
}

static void draw_table_header(int y) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); attron(A_BOLD);
    mvprintw(y, COL_PID, "PID");  mvprintw(y, COL_USER, "USER");
    mvprintw(y, COL_DB, "DB");   mvprintw(y, COL_STATE, "STATE");
    mvprintw(y, COL_TIME, "TIME"); mvprintw(y, COL_CPU, "CPUs%%");
    mvprintw(y, COL_CPUMS, "CPUms"); mvprintw(y, COL_READ, "READ/s"); 
    mvprintw(y, COL_WRITE, "WRITE/s"); mvprintw(y, COL_RSS, "RSS");  
    mvprintw(y, COL_DRSS, "dRSS"); mvprintw(y, COL_BLOCKER, "BLOCKER");
    mvprintw(y, COL_WAIT_TYPE, "WAIT_TYPE"); mvprintw(y, COL_WAIT_EVENT, "WAIT_EVENT");
    mvprintw(y, COL_HOST, "HOST"); attroff(A_BOLD);
}
static void draw_session_row(int y, int row, int width) {
    display_row_t *dr = &display_rows[row];
    pgwa_session *s = dr->ptr;
    int pair = COLOR_PAIR_DEFAULT; int is_lock = 0;

    if (dr->type == ROW_PARALLEL_WORKER) {
        pair = COLOR_PAIR_ACTIVE;   /* тот же цвет, что у комментариев */
    } else {
        if (strstr(s->state, "aborted") != NULL) pair = COLOR_PAIR_ABORTED;
        else if (strstr(s->wait_type, "Lock") != NULL || s->blocker_pid > 0) { pair = COLOR_PAIR_LOCK; is_lock = 1; }
        else if (strcmp(s->state, "active") == 0) pair = COLOR_PAIR_ACTIVE;
    }

    attrset(COLOR_PAIR(pair)); 
    if (is_lock || dr->type == ROW_PARALLEL_WORKER) attron(A_BOLD); 
    if (row == selected_row) attron(A_REVERSE);

    static const int col_starts[] = {
        COL_PID, COL_USER, COL_DB, COL_STATE, COL_TIME, COL_CPU, COL_CPUMS,
        COL_READ, COL_WRITE, COL_RSS, COL_DRSS, COL_BLOCKER, COL_WAIT_TYPE, COL_WAIT_EVENT, COL_HOST
    };
    static const int col_widths[] = { 7, 12, 10, 21, 7, 6, 8, 8, 9, 8, 8, 8, 8, 10, 17, 32 };
    const int ncols = (int)(sizeof(col_starts) / sizeof(col_starts[0]));

    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); for (int c = 0; c < width; c++) mvaddch(y, c, ' ');
    attrset(COLOR_PAIR(pair)); 
    if (is_lock || dr->type == ROW_PARALLEL_WORKER) attron(A_BOLD); 
    if (row == selected_row) attron(A_REVERSE);

    for (int i = 0; i < ncols; i++) {
        int cs = col_starts[i], cw = col_widths[i]; if (cs >= width) break;
        int ce = cs + cw; if (ce > width) ce = width;
        for (int c = cs; c < ce; c++) mvaddch(y, c, ' ');
    }

    if (dr->type == ROW_CLIENT_SESSION) {
        if (COL_PID < width) mvprintw(y, COL_PID, "%-7d", (int)s->pid);
        if (COL_USER < width) mvprintw(y, COL_USER, "%-12.12s", s->user);
        if (COL_DB < width) mvprintw(y, COL_DB, "%-10.10s", s->database);
        const char *state = (strcmp(s->state, "idle in transaction (aborted)") == 0) ? "idle in txn (aborted)" : s->state;
        if (COL_STATE < width) mvprintw(y, COL_STATE, "%-21.21s", state);
        if (COL_TIME < width) mvprintw(y, COL_TIME, "%7.0fs", s->runtime_sec);
        if (COL_CPU < width) mvprintw(y, COL_CPU, "%5.1f%%", s->cpu_pct);
        if (COL_CPUMS < width) mvprintw(y, COL_CPUMS, "%7llu", (unsigned long long)s->cpu_ms); 
        if (COL_READ < width) print_rate_at(y, COL_READ, s->read_per_sec);
        if (COL_WRITE < width) print_rate_at(y, COL_WRITE, s->write_per_sec);
        if (COL_RSS < width) mvprintw(y, COL_RSS, "%6lluM", (unsigned long long)(s->rss_kb / 1024));
        
        if (COL_DRSS < width) {
            int reset_style = 0;
            if (s->rss_delta_kb > 102400 && row != selected_row) { attron(COLOR_PAIR(COLOR_PAIR_LOCK) | A_BOLD); reset_style = 1; }
            print_delta_rss_at(y, COL_DRSS, s->rss_delta_kb);
            if (reset_style) { 
                attrset(COLOR_PAIR(pair)); 
                if (is_lock || dr->type == ROW_PARALLEL_WORKER) attron(A_BOLD); 
                if (row == selected_row) attron(A_REVERSE); 
            }
        }

        if (COL_BLOCKER < width) {
            if (s->blocker_pid > 0) mvprintw(y, COL_BLOCKER, "%7d", (int)s->blocker_pid);
            else mvprintw(y, COL_BLOCKER, "%7s", "");
        }
        if (COL_WAIT_TYPE < width) mvprintw(y, COL_WAIT_TYPE, "%-10.10s", display_wait_type(s));
        if (COL_WAIT_EVENT < width) mvprintw(y, COL_WAIT_EVENT, "%-16.16s", display_wait_event(s));
        if (COL_HOST < width) mvprintw(y, COL_HOST, "%-32.32s", s->host);
    } else {
        if (COL_PID < width) mvprintw(y, COL_PID, " └─%-4d", (int)s->pid);
        if (COL_USER < width) mvprintw(y, COL_USER, "%-12s", "   worker");
        if (COL_DB < width) mvprintw(y, COL_DB, "%-10s", "");
        if (COL_STATE < width) mvprintw(y, COL_STATE, "%-21s", "");
        if (COL_TIME < width) mvprintw(y, COL_TIME, "%7s", "");
        if (COL_CPU < width) mvprintw(y, COL_CPU, "%5.1f%%", s->cpu_pct);
        if (COL_CPUMS < width) mvprintw(y, COL_CPUMS, "%7llu", (unsigned long long)s->cpu_ms);
        if (COL_READ < width) print_rate_at(y, COL_READ, s->read_per_sec);
        if (COL_WRITE < width) print_rate_at(y, COL_WRITE, s->write_per_sec);
        if (COL_RSS < width) mvprintw(y, COL_RSS, "%6lluM", (unsigned long long)(s->rss_kb / 1024));
        if (COL_DRSS < width) print_delta_rss_at(y, COL_DRSS, s->rss_delta_kb);
    }

    attroff(A_REVERSE); attroff(A_BOLD); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
}
#ifndef VERSION
#define VERSION "dev"
#endif

static void draw_footer(int y, int width) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); draw_hline(y, width); move(y + 1, 0);
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw(" Up/Down");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Select Row  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("Sort:[t/c/m/r/w/p]"); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("f");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Filter  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("k");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Kill/Cancel  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("PgUp/PgDn");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Scroll Query  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("Space");      attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Pause  ");
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_BLUE)); printw("q");          attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw(" Quit");

    char v_buf[32];
    int v_len = snprintf(v_buf, sizeof(v_buf), "v:%s", VERSION);
    int v_x = width - v_len;
    if (v_x > 0) {
        attrset(A_NORMAL); 
        mvprintw(y + 1, v_x, "%s", v_buf);
    }
}

static void draw_query_panel(int y, int height, int width) {
    if (height <= 0) return;
    attroff(A_REVERSE); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    wbkgdset(stdscr, COLOR_PAIR(COLOR_PAIR_DEFAULT) | A_NORMAL);
    for (int r = 0; r < height; r++) {
        move(y + r, 0); for (int c = 0; c < width; c++) mvaddch(y + r, c, ' ');
    }
    draw_hline(y, width);
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_DEFAULT)); mvprintw(y + 1, 0, " QUERY PANEL "); attroff(A_BOLD | COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (display_rows_count <= 0) { mvprintw(y + 2, 0, " No active sessions selected."); return; }

    pgwa_session *s = display_rows[selected_row].ptr;
    move(y + 2, 0);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw(" PID: ");    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%d  ", (int)s->pid);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("User: ");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-16.16s  ", s->user);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("DB: ");     attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-16.16s  ", s->database);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Application: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-28.28s ", s->application);
    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Host: ");   attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); printw("%-32.32s ", s->host);
    
    move(y + 3, 0);
    char buf_age[32];

    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw(" Xact Age: "); 
    if (s->xact_age_sec >= 3600.0) attron(COLOR_PAIR(COLOR_PAIR_LOCK) | A_BOLD);
    else attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    format_age_styled(buf_age, sizeof(buf_age), s->xact_age_sec);
    printw("%-9s ", buf_age);

    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("Query Age: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    format_age_styled(buf_age, sizeof(buf_age), s->query_age_sec);
    printw("%-9s ", buf_age);

    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("State Age: "); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    format_age_styled(buf_age, sizeof(buf_age), s->runtime_sec);
    printw("%-9s ", buf_age);

    attron(COLOR_PAIR(COLOR_PAIR_ACTIVE)); printw("State: "); 
    if (strstr(s->state, "aborted") != NULL) attron(COLOR_PAIR(COLOR_PAIR_ABORTED) | A_BOLD);
    else attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    printw("%s", (s->state[0] != '\0') ? s->state : "-");

    int query_y = y + 4, max_query_lines = height - 4; if (max_query_lines <= 0) return;

    size_t wlen = mbstowcs(NULL, s->query, 0); if (wlen == (size_t)-1) return; 
    wchar_t *wquery = malloc((wlen + 1) * sizeof(wchar_t)); if (!wquery) return;
    mbstowcs(wquery, s->query, wlen + 1);

    const wchar_t *p = wquery; int skipped = 0;
    if (query_scroll > 0) {
        int temp_col = 1;
        while (*p && skipped < query_scroll) {
            wchar_t wc = *p++; if (wc == L'\r') continue;
            if (wc == L'\n') { skipped++; temp_col = 1; continue; }
            if (wc == L'\t') wc = L' ';
            int w = wcwidth(wc); if (w < 0) w = 1;
            if (temp_col + w >= width - 1) { skipped++; temp_col = 1; }
            temp_col += w;
        }
    }

    int d_row = 0, col = 1, is_comment = 0; move(query_y + d_row, col);
    while (*p && d_row < max_query_lines) {
        if (col == 1 || is_comment == 0) { if (*p == L'-' && *(p + 1) == L'-') is_comment = 1; }
        wchar_t wc = *p++; if (wc == L'\r') continue;
        if (wc == L'\n') {
            is_comment = 0; d_row++; if (d_row >= max_query_lines) break;
            move(query_y + d_row, 1); col = 1; continue;
        }
        if (wc == L'\t') wc = L' ';
        int w = wcwidth(wc); if (w < 0) w = 1;
        if (col + w >= width - 1) { d_row++; if (d_row >= max_query_lines) break; move(query_y + d_row, 1); col = 1; }

        if (is_comment) { attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE) | A_BOLD); } 
        else {
            const wchar_t *kw[] = {L"SELECT", L"INSERT", L"UPDATE", L"DELETE", L"DO", L"DECLARE", L"BEGIN", L"END", L"$$"};
            int is_keyword = 0; size_t num_keywords = sizeof(kw) / sizeof(kw[0]);
            for (size_t k = 0; k < num_keywords; k++) {
                size_t len = wcslen(kw[k]); if (wcsncmp(p - 1, kw[k], len) == 0) { is_keyword = 1; break; }
            }
            if (is_keyword || wc == L'$') attrset(COLOR_PAIR(COLOR_PAIR_LOCK) | A_BOLD); 
            else attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT)); 
        }
        wchar_t wstr[] = { wc, L'\0' }; mvaddwstr(query_y + d_row, col, wstr); col += w;
    }
    free(wquery); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
}
static void handle_filter_menu(int width, int height) {
    timeout(-1); attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
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
    if (display_rows[selected_row].ptr->pid <= 0) return;

    display_row_t *dr = &display_rows[selected_row];
    pid_t target_pid = dr->ptr->pid;
    int is_worker = (dr->type == ROW_PARALLEL_WORKER);

    if (is_worker && dr->leader_pid > 0) {
        target_pid = dr->leader_pid;   /* киляем лидера, а не воркера */
    }

    timeout(-1); attrset(COLOR_PAIR(COLOR_PAIR_ABORTED)); attron(A_BOLD);
    move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');

    if (is_worker) {
        mvprintw(height - 1, 0, "Worker selected. Will act on LEADER PID %d. c:Cancel, k:Terminate. Choice: ", (int)target_pid);
    } else {
        mvprintw(height - 1, 0, "TARGET PID %d. Actions: c:Cancel backend, k:Kill/Terminate. Choice: ", (int)target_pid);
    }
    attroff(A_BOLD);
    refresh();
    
    int action = getch(); char query[256]; 
    if (action == 'c' || action == 'C') {
        move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
        snprintf(query, sizeof(query), "SELECT pg_cancel_backend(%d);", (int)target_pid);
        postgres_execute(pg, query);
        mvprintw(height - 1, 0, "Sent pg_cancel_backend(%d). Press any key to continue... ", (int)target_pid); refresh(); getch();
    } else if (action == 'k' || action == 'K') {
        move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
        mvprintw(height - 1, 0, "ARE YOU SURE to terminate PID %d? (y/n): ", (int)target_pid);
        refresh(); int confirm = getch();
        if (confirm == 'y' || confirm == 'Y') {
            move(height - 1, 0); for (int i = 0; i < width; i++) mvaddch(height - 1, i, ' ');
            snprintf(query, sizeof(query), "SELECT pg_terminate_backend(%d);", (int)target_pid);
            postgres_execute(pg, query);
            mvprintw(height - 1, 0, "Sent pg_terminate_backend(%d). Press any key to continue... ", (int)target_pid); refresh(); getch();
        }
    }
    timeout(1000); 
}

static int handle_key(int key, int d_rows_count, int width, int height, pgwa_pg_connection *pg) {
    int old_selected = selected_row;
    switch (key) {
        case KEY_UP: if (selected_row > 0) selected_row--; break;
        case KEY_DOWN: if (selected_row < d_rows_count - 1) selected_row++; break;
        case KEY_PPAGE: if (query_scroll >= 5) query_scroll -= 5; else query_scroll = 0; break;
        case KEY_NPAGE: query_scroll += 5; break;
        case KEY_HOME: query_scroll = 0; break;
        case 't': case 'c': case 'm': case 'r': case 'w': case 'p': current_sort = key; break;
        case 'f': case 'F': handle_filter_menu(width, height); break;
        case 'k': case 'K': handle_kill_menu(width, height, pg); break;
        case 'q': case 'Q': return 1;
        case ' ': 
            paused = !paused; 
            break;
    }
    if (old_selected != selected_row) query_scroll = 0;
    return 0;
}

void ui_run(const char *port) {
    pgwa_pg_connection pg = {0}; int total_count = 0;
    setlocale(LC_ALL, "");
    if (postgres_connect(&pg, port) != 0) { fprintf(stderr, "Cannot connect to PostgreSQL\n"); return; }
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(1000);

    if (has_colors()) {
        start_color();
        init_pair(COLOR_PAIR_DEFAULT,  COLOR_WHITE,   COLOR_BLACK);
        init_pair(COLOR_PAIR_ACTIVE,   COLOR_GREEN,   COLOR_BLACK);
        init_pair(COLOR_PAIR_ABORTED,  COLOR_RED,     COLOR_BLACK);
        init_pair(COLOR_PAIR_LOCK,     COLOR_YELLOW,  COLOR_BLACK);
        init_pair(COLOR_PAIR_BLUE,     COLOR_CYAN,    COLOR_BLACK);
        init_pair(COLOR_PAIR_SELECTED, COLOR_BLACK,   COLOR_CYAN);
        /* Попробуем более светлый зелёный для воркеров */
        if (COLORS >= 16) {
            init_color(COLOR_GREEN + 8, 300, 1000, 300);  /* светлее стандартного зелёного */
            init_pair(COLOR_PAIR_WORKER, COLOR_GREEN + 8, COLOR_BLACK);
        } else {
            init_pair(COLOR_PAIR_WORKER, COLOR_GREEN, COLOR_BLACK);
        }
    }
    while (1) {
        int height, width; getmaxyx(stdscr, height, width);
        if (height < 18 || width < 120) {
            erase(); attron(A_BOLD); mvprintw(0, 0, "pg_whoisactive"); attroff(A_BOLD);
            mvprintw(2, 0, "Terminal is too small. Minimum size: 120x18"); refresh();
            int key = getch(); if (key == 'q' || key == 'Q') break; continue;
        }
        if (!paused) {
            if (postgres_read_activity(&pg, sessions, PGWA_MAX_ROWS, &total_count) != 0) break;
            metrics_update(sessions, total_count);
            metrics_update_tps(pg.total_xacts);
        }
        
        int session_count = apply_filter_and_sort(total_count);
        build_display_rows(total_count);
        
        int footer_y = height - 2, query_height = 10, query_y = footer_y - query_height;
        int pause_bar = paused ? 1 : 0;
        int tree_y = query_y - 8, table_y = 2 + pause_bar, table_height = tree_y - table_y;

        erase();
        draw_header(width, session_count, total_count, &pg);
        if (paused) {
            attrset(COLOR_PAIR(COLOR_PAIR_LOCK) | A_BOLD | A_REVERSE);
            for (int c = 0; c < width; c++) mvaddch(2, c, ' ');
            mvprintw(2, (width > 8 ? (width - 6) / 2 : 0), "PAUSED");
            attroff(A_REVERSE | A_BOLD);
        }
        draw_table_header(table_y);
        int visible_rows = table_height - 1; if (visible_rows < 0) visible_rows = 0;
        
        if (display_rows_count <= 0) { selected_row = 0; top_row = 0; }
        else {
            if (selected_row >= display_rows_count) selected_row = display_rows_count - 1;
            if (selected_row < 0) selected_row = 0;
            if (top_row > selected_row) top_row = selected_row;
            if (selected_row >= top_row + visible_rows) top_row = selected_row - visible_rows + 1;
            if (top_row < 0) top_row = 0;
        }

        attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
        for (int i = 0; i < visible_rows; i++) { for (int c = 0; c < width; c++) mvaddch(table_y + 1 + i, c, ' '); }
        int last_drawn = -1;
        for (int i = 0; i < visible_rows; i++) {
            int idx = top_row + i; if (idx >= display_rows_count) break;
            draw_session_row(table_y + 1 + i, idx, width); last_drawn = i;
        }
        attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
        for (int i = last_drawn + 1; i < visible_rows; i++) { for (int c = 0; c < width; c++) mvaddch(table_y + 1 + i, c, ' '); }
        attroff(A_BOLD | A_REVERSE);

        draw_blocking_tree(tree_y, 8, width);
        draw_query_panel(query_y, query_height, width);
        draw_footer(footer_y, width);
        refresh();
        int key = getch();
        if (key != ERR && handle_key(key, display_rows_count, width, height, &pg)) break;
    }
    endwin(); postgres_disconnect(&pg);
}
