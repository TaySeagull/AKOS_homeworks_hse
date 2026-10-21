#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "simulation.h"
#include "output.h"
#include "utils.h"
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
static int parse_arguments(int argc, char **argv, Simulation *sim, const char **config_path, const char **log_path)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(config_path, "config path pointer is NULL", 0);
    SOFT_ASSERT(log_path, "log path pointer is NULL", 0);

    int option = 0;

    // c: config file path, d: delay_ms, s: strategy, l: log file path, h: help
    while ((option = getopt(argc, argv, "c:d:s:l:h")) != -1)
    {
        switch (option)
        {
        case 'c':
            SOFT_ASSERT(optarg != NULL,
                    "config path is NULL",
                    EXIT_FAILURE);
            *config_path = optarg;
            break;

        case 'd':
            SOFT_ASSERT(optarg != NULL,
                    "delay argument is NULL",
                    EXIT_FAILURE);
            sim->delay_ms = atoi(optarg);
            SOFT_ASSERT(sim->delay_ms >= 0, "delay must be non-negative", 0);
            break;

        case 's':
            SOFT_ASSERT(optarg != NULL,
                    "strategy argument is NULL",
                    EXIT_FAILURE);
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
            SOFT_ASSERT(optarg != NULL,
                    "log path is NULL",
                    EXIT_FAILURE);
            *log_path = optarg;
            break;
    
        case 'h':
            // has no optarg
            print_usage(argv[0]);
            return 0;

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
 * @brief Loads the configuration and runs the simulation
 * @details This is the start point of the application
 * It loads the configuration from the specified file, opens the log file,
 * checks command-line arguments.
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
        argc,
        argv,
        &sim,
        &config_path,
        &log_path
    );

    if (!result)
        return 1;

    if (!load_config(&sim, config_path))
        return 1;

    // Reapply command-line arguments
    optind = 1;

    result = parse_arguments(
        argc,
        argv,
        &sim,
        &config_path,
        &log_path
    );

    if (!result)
        return 1;

    if (!validate_simulation(&sim))
        return 1;

    if (!open_log_file(&sim, log_path))
        return 1;

    install_signal();

    run_simulation(&sim);

    fclose(sim.log);

    return 0;
}
