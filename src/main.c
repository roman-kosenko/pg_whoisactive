#include <stdio.h>
#include <string.h>

#include "plain.h"
#include "ui.h"

#ifndef VERSION
#define VERSION "dev"
#endif

static void print_usage(const char *program)
{
    printf("Usage: %s [port] [--plain]\n", program);
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
    const char *port = NULL;

    // Основной цикл обработки рабочих параметров (порт, plain-режим)
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--plain") == 0)
        {
            plain_mode = 1;
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

    if (plain_mode)
    {
        plain_run(port);
    }
    else
    {
        ui_run(port);
    }

    return 0;
}
