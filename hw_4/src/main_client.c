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

#ifdef IPC_NET
#include "net.h"
#endif

#ifndef CLIENT_ROLE
#error "CLIENT_ROLE must be defined"
#endif

#define DEFAULT_WAIT 30 // seconds to wait for the clock / for the server

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

#ifdef IPC_NET

/**
 * @brief Prints the usage of a network client
 * @param program The name of the program
 */
static void usage(const char *program)
{
    fd_printf(STDOUT_FILENO,
              "Usage: %s [-H host] [-p port] [-w seconds]%s [-l logfile] [-h]\n"
              "  -H host    address or name of the server (default 127.0.0.1)\n"
              "  -p port    TCP port of the server (default %d)\n"
              "  -w sec     how long to wait for a server that is not started yet (default %d)\n"
              "%s"
              "  -l file    write the lines of this program into a local log file as well\n"
              "  -h         show this help\n"
              "Role of this program: %s. It shows only what it receives from the server.\n"
              "Ctrl+C, Ctrl+\\, SIGTERM and SIGUSR1 are not sent to the server as signals\n"
              "(it may run on another computer): the client turns them into a frame of the\n"
              "protocol, and the server decides what to do with the request.\n"
              "The program can be stopped and started again at any time: the server keeps\n"
              "the messages of its role and sends the whole state to the new client.\n",
              program, CLIENT_ROLE == ROLE_TEAM ? " [-i team_id]" : "", NET_DEFAULT_PORT, DEFAULT_WAIT,
              CLIENT_ROLE == ROLE_TEAM ? "  -i id      play the team with this id (default: first free team)\n" : "",
              role_text(CLIENT_ROLE));
}

/**
 * @brief Entry point of a network client
 * @param argc Number of arguments
 * @param argv Arguments
 * @return 0 on success, 1 on a wrong start, 2 if the server was lost
 */
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *log_path = NULL;
    int port = NET_DEFAULT_PORT;
    int wait_seconds = DEFAULT_WAIT;
    int team_id = 0;
    int number = 0;
    int option = 0;

    while ((option = getopt(argc, argv, CLIENT_ROLE == ROLE_TEAM ? "H:p:w:i:l:h" : "H:p:w:l:h")) != -1)
    {
        switch (option)
        {
        case 'H':
            if (strlen(optarg) == 0 || strlen(optarg) >= NET_HOST_LEN)
            {
                fprintf(stderr, "Invalid host '%s'\n", optarg);
                return 1;
            }
            host = optarg;
            break;

        case 'p':
            if (!parse_int_strict(optarg, &number) || number < 1 || number > 65535)
            {
                fprintf(stderr, "Invalid port '%s': a number between 1 and 65535 is expected\n", optarg);
                return 1;
            }
            port = number;
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

        case 'l':
            log_path = optarg;
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

    // the handlers only remember the request of the user; the client sends it to
    // the server, because a signal cannot be sent to another computer
    signals_install_server();

    ipc_set_endpoint(host, port);

    int idx = team_id;
    Simulation *sim = ipc_client_attach(CLIENT_ROLE, &idx, NULL, wait_seconds);

    if (sim == NULL)
        return 1;

    // the central log is written by the server; a local copy is optional
    int fd = -1;

    if (log_path != NULL)
    {
        fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

        if (fd < 0)
            perror(log_path); // the client can work without a local log
    }

    set_log_fd(fd);

    if (CLIENT_ROLE == ROLE_TEAM)
        fd_printf(STDOUT_FILENO, "[%s] pid %d plays team B%d, server %s:%d\n", role_text(CLIENT_ROLE), (int)getpid(),
                  sim->teams[idx].id, host, port);
    else
        fd_printf(STDOUT_FILENO, "[%s] pid %d registered, server %s:%d\n", role_text(CLIENT_ROLE), (int)getpid(), host,
                  port);

    role_loop(sim, CLIENT_ROLE, idx);

    int lost = ipc_server_lost();

    if (lost)
        fd_printf(STDOUT_FILENO,
                  "[%s] pid %d: the connection to the server is gone. The server keeps the\n"
                  "        messages of this role: start this program again to go on.\n",
                  role_text(CLIENT_ROLE), (int)getpid());
    else
        fd_printf(STDOUT_FILENO, "[%s] pid %d: the shift is over, exiting\n", role_text(CLIENT_ROLE), (int)getpid());

    ipc_client_detach(sim);

    if (fd >= 0)
        close(fd);

    return lost ? 2 : 0;
}

#else

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

#endif
