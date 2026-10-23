/**
 * @file main.c- 2d hw update
 */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "simulation.h"
#include "output.h"
#include "utils.h"
#include "threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

/**
 * @brief Parses command-line arguments
 * @param argc Number of command-line arguments
 * @param argv Array of command-line arguments
 * @param sim Pointer to the simulation structure
 * @param config_path Pointer to the configuration file path
 * @param log_path Pointer to the log file path
 * @return 1 on success, 0 on failure
 */
static int parse_arguments(
    int argc,
    char **argv,
    Simulation *sim,
    const char **config_path,
    const char **log_path)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(config_path, "config path pointer is NULL", 0);
    SOFT_ASSERT(log_path, "log path pointer is NULL", 0);

    int option = 0;

    while ((option = getopt(argc, argv, "c:d:s:l:h")) != -1)
    {
        switch (option)
        {
        case 'c':
            SOFT_ASSERT(optarg != NULL, "config path is NULL", 0);
            *config_path = optarg;
            break;

        case 'd':
            SOFT_ASSERT(optarg != NULL, "delay argument is NULL", 0);
            sim->delay_ms = atoi(optarg);
            SOFT_ASSERT(sim->delay_ms >= 0, "delay must be non-negative", 0);
            break;

        case 's':
            SOFT_ASSERT(optarg != NULL, "strategy argument is NULL", 0);

            if (!strcasecmp(optarg, "fastest"))
                sim->strategy = STRATEGY_FASTEST;
            else if (!strcasecmp(optarg, "nearest"))
                sim->strategy = STRATEGY_NEAREST;
            else
            {
                fprintf(stderr, "Unknown strategy: %s\n", optarg);
                return 0;
            }
            break;

        case 'l':
            SOFT_ASSERT(optarg != NULL, "log path is NULL", 0);
            *log_path = optarg;
            break;

        case 'h':
            print_usage(argv[0]);
            return 2;

        default:
            print_usage(argv[0]);
            return 0;
        }
    }

    return 1;
}

/**
 * @brief Opens the log file
 * @param sim Pointer to the simulation structure
 * @param path Path to the log file
 * @return 1 on success, 0 on failure
 */
static int open_log_file(Simulation *sim, const char *path)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(path, "log path is NULL", 0);

    sim->log = fopen(path, "w");
    SOFT_ASSERT(sim->log, "cannot open log file", 0);

    return 1;
}

/**
 * @brief Initializes the simulation, runs it, and releases resources
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

    int result = parse_arguments(
        argc, argv, &sim, &config_path, &log_path
    );

    if (result == 2)
        return 0;

    if (!result)
        return 1;

    if (!load_config(&sim, config_path))
        return 1;

    optind = 1;

    result = parse_arguments(
        argc, argv, &sim, &config_path, &log_path
    );

    if (result == 2)
        return 0;

    if (!result)
        return 1;

    if (!validate_simulation(&sim))
        return 1;

    if (!open_log_file(&sim, log_path))
        return 1;
    
    // start threads

    if (!init_thread_sync(&sim))
    {
        fclose(sim.log);
        return 1;
    }

    if (!start_team_threads(&sim))
    {
        destroy_thread_sync(&sim);
        fclose(sim.log);
        return 1;
    }

    install_signal();

    run_simulation(&sim);

    stop_team_threads(&sim);
    
    destroy_thread_sync(&sim);

    fclose(sim.log);

    return 0;
}