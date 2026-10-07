#define _POSIX_C_SOURCE 200809L
#include "utils_extra.h"
#include "utils.h"
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/**
 * @brief Returns the name of a specialization
 * @param specialization The specialization
 * @return The name of the specialization
 */
const char *spec_name(Specialization specialization)
{
    static const char *name[] = {"MEDIC", "REANIM", "FIRE", "POLICE"};

    SOFT_ASSERT(specialization >= 0 && specialization < SPEC_COUNT, "invalid specialization", "UNKNOWN");

    return name[specialization];
}

/**
 * @brief Returns the name of a call state
 * @param call_state The call state
 * @return The name of the call state
 */
const char *call_state_name(CallState call_state)
{
    static const char *name[] =
    {
        "PENDING",
        "WAITING",
        "DISPATCHED",
        "EN_ROUTE",
        "IN_SERVICE",
        "COMPLETED",
        "CANCELLED",
        "UNSERVED"
    };

    SOFT_ASSERT(
        call_state >= CALL_PENDING && call_state <= CALL_UNSERVED,
        "invalid call state",
        "UNKNOWN"
    );

    return name[call_state];
}

/**
 * @brief Returns the name of a team state
 * @param team_state The team state
 * @return The name of the team state
 */
const char *team_state_name(TeamState team_state)
{
    static const char *name[] = {"AVAILABLE", "EN_ROUTE", "AT_SCENE", "RETURNING"};

    SOFT_ASSERT(team_state >= TEAM_AVAILABLE && team_state <= TEAM_RETURNING, "invalid team state", "UNKNOWN");

    return name[team_state];
}

/**
 * @brief Returns the name of a strategy
 * @param strategy The strategy
 * @return The name of the strategy
 */
const char *strategy_name(Strategy strategy)
{
    SOFT_ASSERT(strategy == STRATEGY_NEAREST || strategy == STRATEGY_FASTEST, "invalid strategy", "UNKNOWN");

    return strategy == STRATEGY_FASTEST ? "FASTEST" : "NEAREST";
}

/**
 * @brief Parses a specialization name and returns its enum value
 * @param spec The specialization name
 * @return The specialization enum value, or -1 if not found
 */
int parse_spec(const char *spec)
{
    SOFT_ASSERT(spec, "specialization text is NULL", -1);

    if (!strcasecmp(spec, "MEDIC"))
        return SPEC_MEDIC;

    if (!strcasecmp(spec, "REANIM"))
        return SPEC_REANIM;

    if (!strcasecmp(spec, "FIRE"))
        return SPEC_FIRE;

    if (!strcasecmp(spec, "POLICE"))
        return SPEC_POLICE;

    return -1;
}

/**
 * @brief Finds the index of a district by its name
 * @param sim Pointer to the simulation structure
 * @param name The name of the district to find
 * @return The index of the district, or -1 if not found
 */
int district_index(const Simulation *sim, const char *name)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);
    SOFT_ASSERT(name, "district name is NULL", -1);

    for (int idx = 0; idx < sim->district_count; idx++)
        if (!strcmp(sim->districts[idx].name, name))
            return idx;

    return -1;
}

/**
 * @brief Finds the index of a call by its ID
 * @param sim Pointer to the simulation structure
 * @param id The ID of the call to find
 * @return The index of the call, or -1 if not found
 */
int find_call_index_by_id(const Simulation *sim, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].id == id)
            return idx;

    return -1;
}

/**
 * @brief Finds the index of a team by its ID
 * @param sim Pointer to the simulation structure
 * @param id The ID of the team to find
 * @return The index of the team, or -1 if not found
 */
int find_team_index_by_id(const Simulation *sim, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].id == id)
            return idx;

    return -1;
}

/**
 * @brief Logs an event to the simulation output
 * @param sim Pointer to the simulation structure
 * @param fmt The format string for the log message
 * @param ... The arguments for the format string
 * @return 1 on success, 0 on failure
 */
int log_event(Simulation *sim, const char *fmt, ...)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(fmt, "log format is NULL", 0);

    va_list ap;        // Variable argument list
    va_start(ap, fmt); // Initialize the variable argument list

    // current time of the simulation
    printf("\n[%02d:%02d:%02d] ", sim->now / 3600, (sim->now / 60) % 60, sim->now % 60);
    
    vprintf(fmt, ap);
    printf("\n");

    if (sim->log)
    {
        va_end(ap); // start again

        va_start(ap, fmt);
        fprintf(sim->log, "[%02d:%02d:%02d] ", sim->now / 3600, (sim->now / 60) % 60, sim->now % 60);
        vfprintf(sim->log, fmt, ap);
        fprintf(sim->log, "\n");
    }

    va_end(ap); // real end

    fflush(stdout); // immediate output to console

    if (sim->log)
        fflush(sim->log); // immediate output to log file

    return 1;
}

/**
 * @brief Pauses the simulation for a specified amount of time
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int sleep_visual(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    if (sim->delay_ms <= 0)
        return 1;

    struct timespec timesp = {sim->delay_ms / 1000, (long)(sim->delay_ms % 1000) * 1000000L};
    
    nanosleep(&timesp, NULL);

    return 1;
}
