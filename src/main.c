/* SPDX-License-Identifer: GPL-2.0-or-later

Copyright (C) 2014  Vyacheslav Trushkin
Copyright (C) 2020  Boian Bonev

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "iotop.h"

#include <pwd.h>
#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

static const char *progname = NULL;

config_t config;
params_t params;

inline void
init_params(void)
{
    params.iter = -1;
    params.delay = 1;
    params.pid = -1;
    params.user_id = -1;
}

static const char str_opt[] = "boPaktqHc";

static inline void
print_help(void)
{
    printf(
        "Usage: %s [OPTIONS]\n\n"
        "DISK READ and DISK WRITE are the block I/O bandwidth used during the sampling\n"
        "period. SWAPIN and IO are the percentages of time the thread spent respectively\n"
        "while swapping in and waiting on I/O more generally. PRIO is the I/O priority\n"
        "at which the thread is running (set using the ionice command).\n\n"
        "Controls: left and right arrows to change the sorting column, r to invert the\n"
        "sorting order, o to toggle the --only option, p to toggle the --processes\n"
        "option, a to toggle the --accumulated option, i to change I/O priority, q to\n"
        "quit, any other key to force a refresh.\n\n"
        "Options:\n"
        "  --version             show program's version number and exit\n"
        "  -h, --help            show this help message and exit\n"
        "  -o, --only            only show processes or threads actually doing I/O\n"
        "  -b, --batch           non-interactive mode\n"
        "  -n NUM, --iter=NUM    number of iterations before ending [infinite]\n"
        "  -d SEC, --delay=SEC   delay between iterations [1 second]\n"
        "  -p PID, --pid=PID     processes/threads to monitor [all]\n"
        "  -u USER, --user=USER  users to monitor [all]\n"
        "  -P, --processes       only show processes, not all threads\n"
        "  -a, --accumulated     show accumulated I/O instead of bandwidth\n"
        "  -k, --kilobytes       use kilobytes instead of a human friendly unit\n"
        "  -t, --time            add a timestamp on each line (implies --batch)\n"
        "  -c, --fullcmdline     show full command line\n"
        "  -q, --quiet           suppress some lines of header (implies --batch)\n"
        "  --no-help             suppress listing of shortcuts\n",
        progname
    );
}

static inline void
parse_args(int argc, char *argv[])
{
    init_params();
    memset(&config, 0, sizeof(config));

    while (1)
    {
        static struct option long_options[] =
        {
            {"version",     no_argument, NULL, 'v'},
            {"help",        no_argument, NULL, 'h'},
            {"batch",       no_argument, NULL, 'b'},
            {"only",        no_argument, NULL, 'o'},
            {"iter",        required_argument, NULL, 'n'},
            {"delay",       required_argument, NULL, 'd'},
            {"pid",         required_argument, NULL, 'p'},
            {"user",        required_argument, NULL, 'u'},
            {"processes",   no_argument, NULL, 'P'},
            {"accumulated", no_argument, NULL, 'a'},
            {"kilobytes",   no_argument, NULL, 'k'},
            {"timestamp",   no_argument, NULL, 't'},
            {"quiet",       no_argument, NULL, 'q'},
            {"no-help",     no_argument, NULL, 'H'},
            {"fullcmdline", no_argument, NULL, 'c'},
            {NULL, 0, NULL, 0}
        };

        int c = getopt_long(argc, argv, "vhbon:d:p:u:Paktqc",
                            long_options, NULL);

        if (c == -1)
            break;

        switch (c)
        {
        case 'v':
            printf("%s %s\n", argv[0], VERSION);
            exit(EXIT_SUCCESS);
        case 'h':
            print_help();
            exit(EXIT_SUCCESS);
        case 'o':
        case 'b':
        case 'P':
        case 'a':
        case 'k':
        case 't':
        case 'q':
        case 'H':
        case 'c':
            config.opts[(unsigned int) (strchr(str_opt, c) - str_opt)] = 1;
            break;
        case 'n':
            params.iter = atoi(optarg);
            break;
        case 'd':
            params.delay = atoi(optarg);
            break;
        case 'p':
            params.pid = atoi(optarg);
            break;
        case 'u':
            if (isdigit(optarg[0]))
                params.user_id = atoi(optarg);
            else
            {
                struct passwd *pwd = getpwnam(optarg);
                if (!pwd)
                {
                    fprintf(stderr, "%s: user %s not found\n",
                            progname, optarg);
                    exit(EXIT_FAILURE);
                }
                params.user_id = pwd->pw_uid;
            }
            break;
        default:
            fprintf(stderr, "%s: unknown option\n", progname);
            exit(EXIT_FAILURE);
        }
    }
}

inline int
filter1(struct xxxid_stats *s)
{
    if ((params.user_id != -1) && (s->euid != params.user_id))
        return 1;

    if ((params.pid != -1) && (s->tid != params.pid))
        return 1;

    return 0;
}

inline void
sig_handler(int signo)
{
    if (signo == SIGINT)
    {
        nl_term();
        if (!config.f.batch_mode)
            view_curses_finish();

        exit(EXIT_SUCCESS);
    }
}

int
main(int argc, char *argv[])
{
    progname = argv[0];

    parse_args(argc, argv);
    if (system_checks())
        return EXIT_FAILURE;

    setlocale(LC_ALL, "");
    nl_init();

    if (signal(SIGINT, sig_handler) == SIG_ERR)
        perror("signal");

    struct xxxid_stats_arr *ps = NULL;
    struct xxxid_stats_arr *cs = NULL;
    struct act_stats act = {0};

    if (config.f.timestamp || config.f.quiet)
        config.f.batch_mode = 1;

    view_callback view = view_batch;
    how_to_sleep do_sleep = sleep;

    if (!config.f.batch_mode)
    {
        view = view_curses;
        do_sleep = curses_sleep;
    }

    do
    {
        cs = fetch_data(config.f.processes, filter1);
        get_vm_counters(&act.read_bytes,&act.write_bytes);
        act.ts_c = monotime();
        view(cs, ps, &act);

        if (ps)
            arr_free(ps);

        ps = cs;
        act.read_bytes_o = act.read_bytes;
        act.write_bytes_o = act.write_bytes;
        act.ts_o = act.ts_c;
        act.have_o = 1;

        if ((params.iter > -1) && ((--params.iter) == 0))
            break;
    }
    while (!do_sleep(params.delay));

    arr_free(cs);
    sig_handler(SIGINT);

    return 0;
}
