#include <ncurses.h>

#include "procfs.h"
#include <unistd.h>
#include <inttypes.h>

int main(void)
{
    initscr();

    noecho();

    cbreak();

    uint64_t cpu;
    uint64_t rss;
    uint64_t rd;
    uint64_t wr;

    procfs_read(getpid(),
                &cpu,
                &rss,
                &rd,
                &wr);

    mvprintw(1,2,"pg_whoisactive");

    mvprintw(3,2,"Current PID : %d",getpid());

    mvprintw(4,2,"RSS         : %" PRIu64 " KB", rss);

    mvprintw(5,2,"CPU ticks   : %" PRIu64, cpu);

    refresh();

    getch();

    endwin();

    return 0;
}