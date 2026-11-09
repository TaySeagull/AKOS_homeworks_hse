#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "ipc.h"
#include "roles.h"
#include "signals.h"
#include "utils.h"
#include "utils_extra.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CLIENT_ROLE
#error "CLIENT_ROLE must be defined"
#endif

#define DEFAULT_WAIT 30 // seconds to wait for the clock

/**
 * @brief Name of the role for messages
 * @param role The role
 * @return The name of the role
 */
static const char *role_text(Role role)
{
    switch (role)
    {
    case ROLE_DISPATCHER:
        return "dispatcher";
    case ROLE_TEAM:
        return "team";
    case ROLE_CALLERS:
        return "callers (family of all calls)";
    default:
        return "?";
    }
}

/**
 * @brief Prints the usage of the client
 * @param program The name of the program
 */
static void usage(const char *program)
{
    fd_printf(STDOUT_FILENO,
              "Usage: %s [-n session] [-w seconds]%s [-h]\n"
              "  -n name    session name, the same as for the clock (default main)\n"
              "  -w sec     how long to wait for the clock (default %d)\n"
              "%s"
              "  -h         show this help\n"
              "Role of this program: %s. Signals (Ctrl+C, Ctrl+\\, SIGTERM, SIGUSR1)\n"
              "are forwarded to the clock, it decides what to do.\n",
              program, CLIENT_ROLE == ROLE_TEAM ? " [-i team_id]" : "", DEFAULT_WAIT,
              CLIENT_ROLE == ROLE_TEAM ? "  -i id      play the team with this id (default: first free team)\n" : "",
              role_text(CLIENT_ROLE));
}

/**
 * @brief Entry point of a client
 * @param argc Number of arguments
 * @param argv Arguments
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *session = "main";
    int wait_seconds = DEFAULT_WAIT;
    int team_id = 0;
    int option = 0;

    while ((option = getopt(argc, argv, CLIENT_ROLE == ROLE_TEAM ? "n:w:i:h" : "n:w:h")) != -1)
    {
        switch (option)
        {
        case 'n':
            if (strlen(optarg) == 0 || strlen(optarg) >= SESSION_LEN || strchr(optarg, '/'))
            {
                fprintf(stderr, "Invalid session name '%s'\n", optarg);
                return 1;
            }
            session = optarg;
            break;

        case 'w':
            if (!parse_int_strict(optarg, &wait_seconds) || wait_seconds < 0)
            {
                fprintf(stderr, "Invalid waiting time '%s'\n", optarg);
                return 1;
            }
            break;

        case 'i':
            if (!parse_int_strict(optarg, &team_id) || team_id <= 0)
            {
                fprintf(stderr, "Invalid team id '%s'\n", optarg);
                return 1;
            }
            break;

        case 'h':
            usage(argv[0]);
            return 0;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return 1;
    }

    // until the clock is known Ctrl+C simply stops the waiting
    signals_install_server();

    int idx = team_id;
    Simulation *sim = ipc_client_attach(CLIENT_ROLE, &idx, session, wait_seconds);

    if (sim == NULL)
        return 1;

    // from now on the signals are the business of the clock
    signals_install_client(sim->clock_pid);

    int fd = open(sim->log_path, O_WRONLY | O_APPEND);
    if (fd < 0)
        perror(sim->log_path); // the simulation can go on without the log
    set_log_fd(fd);

    if (CLIENT_ROLE == ROLE_TEAM)
        fd_printf(STDOUT_FILENO, "[%s] pid %d plays team B%d (session '%s')\n", role_text(CLIENT_ROLE), (int)getpid(),
                  sim->teams[idx].id, session);
    else
        fd_printf(STDOUT_FILENO, "[%s] pid %d registered (session '%s')\n", role_text(CLIENT_ROLE), (int)getpid(),
                  session);

    role_loop(sim, CLIENT_ROLE, idx);

    fd_printf(STDOUT_FILENO, "[%s] pid %d: the shift is over, exiting\n", role_text(CLIENT_ROLE), (int)getpid());

    ipc_client_detach(sim);

    if (fd >= 0)
        close(fd);

    return 0;
}
