#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "simulation.h"
#include "output.h"
#include "utils.h"
#include "utils_extra.h"
#include "threads.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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
 * @return ARGS_OK, ARGS_HELP or ARGS_ERROR
 */
static int parse_arguments(int argc, char **argv, Simulation *sim, const char **config_path, const char **log_path)
{
    SOFT_ASSERT(sim, "simulation is NULL", ARGS_ERROR);
    SOFT_ASSERT(config_path, "config path pointer is NULL", ARGS_ERROR);
    SOFT_ASSERT(log_path, "log path pointer is NULL", ARGS_ERROR);

    int option = 0;
    int number = 0;

    optind = 1; // restart getopt() from the first argument
    opterr = 1;

    while ((option = getopt(argc, argv, "c:d:s:l:t:r:h")) != -1)
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
 * @param sim Pointer to the simulation structure
 * @param path Path to the log file
 * @return 1 on success, 0 on failure
 */
static int open_log_file(Simulation *sim, const char *path)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(path, "log path is NULL", 0);

    sim->log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (sim->log_fd < 0)
    {
        perror(path);
        return 0;
    }

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
    Simulation sim;

    set_defaults(&sim);

    const char *config_path = "config.txt";
    const char *log_path = "dispatch.log";

    int result = parse_arguments(argc, argv, &sim, &config_path, &log_path);

    if (result != ARGS_OK)
        return result == ARGS_HELP ? 0 : 1;

    if (!load_config(&sim, config_path))
        return 1;

    // Reapply command-line arguments: they have priority over the file
    result = parse_arguments(argc, argv, &sim, &config_path, &log_path);

    if (result != ARGS_OK)
        return result == ARGS_HELP ? 0 : 1;

    if (!validate_simulation(&sim))
        return 1;

    if (!open_log_file(&sim, log_path))
        return 1;

    // the signals are blocked before any thread is created
    if (!block_signals() || !init_thread_sync(&sim))
    {
        close(sim.log_fd);
        return 1;
    }

    if (!start_service_threads(&sim))
    {
        destroy_thread_sync(&sim);
        close(sim.log_fd);
        return 1;
    }

    // one thread for every configured call (the caller)
    for (int idx = 0; idx < sim.call_count; idx++)
    {
        if (!start_call_thread(&sim, idx))
        {
            fprintf(stderr, "cannot create the thread of call #%d\n", sim.calls[idx].id);
            stop_all_threads(&sim);
            destroy_thread_sync(&sim);
            close(sim.log_fd);
            return 1;
        }
    }

    int ok = run_simulation(&sim); // the main thread works as the clock

    stop_all_threads(&sim);

    // all threads are joined, the summary sees the final state
    print_summary(&sim);

    destroy_thread_sync(&sim);

    close(sim.log_fd);

    return ok ? 0 : 1;
}
