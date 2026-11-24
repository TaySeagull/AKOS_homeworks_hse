#define _XOPEN_SOURCE 700

#include "config.h"
#include "ipc.h"
#include "output.h"
#include "signals.h"
#include "simulation.h"
#include "utils.h"
#include "utils_extra.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ARGS_ERROR 0 // wrong arguments, exit with code 1
#define ARGS_OK 1    // continue
#define ARGS_HELP 2  // -h was given, exit with code 0

/**
 * @brief Parses command-line arguments
 * @param argc Number of command-line arguments
 * @param argv Array of command-line arguments
 * @param sim Pointer to the simulation structure
 * @param config_path Pointer to the configuration file path
 * @param log_path Pointer to the log file path
 * @param session Pointer to the session name (prefix of the IPC names)
 * @return ARGS_OK, ARGS_HELP or ARGS_ERROR
 */
static int parse_arguments(int argc, char **argv, Simulation *sim, const char **config_path, const char **log_path,
                           const char **session)
{
    SOFT_ASSERT(sim, "simulation is NULL", ARGS_ERROR);
    SOFT_ASSERT(config_path, "config path pointer is NULL", ARGS_ERROR);
    SOFT_ASSERT(log_path, "log path pointer is NULL", ARGS_ERROR);

    int option = 0;
    int number = 0;

    optind = 1; // restart getopt() from the first argument
    opterr = 1;

    while ((option = getopt(argc, argv, "c:d:s:l:t:r:n:h")) != -1)
    {
        switch (option)
        {
        case 'c':
            *config_path = optarg;
            break;

        case 'd':
            if (!parse_int_strict(optarg, &number) || number < 0)
            {
                fprintf(stderr, "Invalid delay '%s': a non-negative integer (ms) is expected\n", optarg);
                return ARGS_ERROR;
            }
            sim->delay_ms = number;
            break;

        case 's':
            if (!strcasecmp(optarg, "fastest"))
                sim->strategy = STRATEGY_FASTEST;
            else if (!strcasecmp(optarg, "nearest"))
                sim->strategy = STRATEGY_NEAREST;
            else
            {
                fprintf(stderr, "Unknown strategy: %s (use nearest or fastest)\n", optarg);
                return ARGS_ERROR;
            }
            break;

        case 'l':
            *log_path = optarg;
            break;

        case 't':
            if (!parse_int_strict(optarg, &number) || number < 0)
            {
                fprintf(stderr, "Invalid shift limit '%s': minutes >= 0 are expected (0 = unlimited)\n", optarg);
                return ARGS_ERROR;
            }
            sim->shift_limit = number;
            break;

        case 'r':
            if (!parse_int_strict(optarg, &number) || number < 0)
            {
                fprintf(stderr, "Invalid seed '%s': a non-negative integer is expected\n", optarg);
                return ARGS_ERROR;
            }
            sim->seed = (unsigned int)number;
            sim->random_calls = 1;
            break;

        case 'n':
            if (strlen(optarg) == 0 || strlen(optarg) >= SESSION_LEN || strchr(optarg, '/'))
            {
                fprintf(stderr, "Invalid session name '%s'\n", optarg);
                return ARGS_ERROR;
            }
            *session = optarg;
            break;

        case 'h':
            print_usage(argv[0]);
            return ARGS_HELP;

        default: // unknown option, getopt() has already printed the message
            print_usage(argv[0]);
            return ARGS_ERROR;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        print_usage(argv[0]);
        return ARGS_ERROR;
    }

    return ARGS_OK;
}

/**
 * @brief Opens the log file with the open(2) system call
 * @details O_APPEND: the processes of the independent versions open the same
 * file, every write(2) of a line is appended to its end.
 * @param path Path to the log file
 * @return 1 on success, 0 on failure
 */
static int open_log_file(const char *path)
{
    SOFT_ASSERT(path, "log path is NULL", 0);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    if (fd < 0)
    {
        perror(path);
        return 0;
    }

    set_log_fd(fd);
    return 1;
}

/**
 * @brief Loads the configuration and runs the simulation
 * @param argc Number of command-line arguments
 * @param argv Array of command-line arguments
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    static Simulation loaded; // the model is copied into the IPC objects later

    set_defaults(&loaded);

    const char *config_path = "config.txt";
    const char *log_path = "dispatch.log";
    const char *session = "main";

    int result = parse_arguments(argc, argv, &loaded, &config_path, &log_path, &session);

    if (result != ARGS_OK)
        return result == ARGS_HELP ? 0 : 1;

    if (!load_config(&loaded, config_path))
        return 1;

    // Reapply command-line arguments: they have priority over the file
    result = parse_arguments(argc, argv, &loaded, &config_path, &log_path, &session);

    if (result != ARGS_OK)
        return result == ARGS_HELP ? 0 : 1;

    if (!validate_simulation(&loaded))
        return 1;

    if (!open_log_file(log_path))
        return 1;

    // the clients of the independent versions open the same log file: absolute path
    char resolved[PATH_MAX];
    const char *source = realpath(log_path, resolved) != NULL ? resolved : log_path;
    size_t length = strlen(source);

    if (length >= sizeof(loaded.log_path))
        length = sizeof(loaded.log_path) - 1;

    memcpy(loaded.log_path, source, length);
    loaded.log_path[length] = '\0';

    ipc_remember_config(config_path);

    // handlers are installed before any child exists
    if (!signals_install_server())
    {
        close(get_log_fd());
        return 1;
    }

    Simulation *sim = ipc_server_init(&loaded, session);

    if (sim == NULL)
    {
        close(get_log_fd());
        return 1;
    }

    int ok = 0;

    if (ipc_start_processes(sim))
    {
        int started = 1;

        // one process for every configured call (or the callers family plays them)
        for (int idx = 0; idx < sim->call_count; idx++)
        {
            if (!ipc_start_call(sim, idx))
            {
                fprintf(stderr, "cannot start the process of call #%d\n", sim->calls[idx].id);
                started = 0;
                break;
            }
        }

        if (started)
            ok = run_simulation(sim); // this process works as the clock
    }
    else
        fprintf(stderr, "The simulation was not started: the processes are not ready\n");

    ipc_stop_all(sim);

    // all processes have finished, the summary sees the final state
    if (ok)
        print_summary(sim);

    // exit code: 0 - normal end (also after Ctrl+C), 2 - a process died, 1 - error
    int code = !ok ? 1 : (sim->stop_reason == STOP_PROCESS_FAILURE ? 2 : 0);

    ipc_server_cleanup(); // the shared memory is unmapped here, sim is not valid any more

    close(get_log_fd());

    return code;
}
