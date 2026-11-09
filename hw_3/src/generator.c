#define _POSIX_C_SOURCE 200809L
#include "generator.h"
#include "simulation.h"
#include "threads.h"
#include "utils.h"
#include "utils_extra.h"
#include <stdlib.h>

/**
 * @brief Returns a pseudo-random number in [low, high]
 * @details rand_r() keeps its state in sim->rng, so the same seed always
 * produces the same sequence of calls (reproducible runs).
 * @param sim Pointer to the simulation structure
 * @param low Lower bound
 * @param high Upper bound
 * @return Random number
 */
static int random_between(Simulation *sim, int low, int high)
{
    if (high <= low)
        return low;

    return low + (int)(rand_r(&sim->rng) % (unsigned)(high - low + 1));
}

/**
 * @brief Finds a slot for a new call
 * @param sim Pointer to the simulation structure
 * @return Index of the slot or -1 if all calls are still active
 */
static int find_free_call_slot(Simulation *sim)
{
    if (sim->call_count < MAX_CALLS)
        return sim->call_count++;

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        CallState state = sim->calls[idx].state;
        int closed = state == CALL_COMPLETED || state == CALL_CANCELLED || state == CALL_UNSERVED;

        // the thread of a closed call is idle, it is stopped and joined
        if (closed)
        {
            join_call_thread(sim, idx);
            return idx;
        }
    }

    return -1;
}

/**
 * @brief Creates a random call that arrives right now
 * @param sim Pointer to the simulation structure
 * @return Index of the created call, -1 if the call table is full
 */
int create_random_call(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);

    int idx = find_free_call_slot(sim);
    int id = sim->next_call_id++;

    if (idx < 0)
    {
        sim->received++;
        sim->unserved++;
        log_event(sim, "CALL #%03d REJECTED: all %d call slots are busy, recorded as unserved", id, MAX_CALLS);
        return -1;
    }

    // specializations that really exist among the teams
    int specs[SPEC_COUNT];
    int spec_count = 0;
    for (int spec = 0; spec < SPEC_COUNT; spec++)
        for (int team = 0; team < sim->team_count; team++)
            if ((int)sim->teams[team].spec == spec)
            {
                specs[spec_count++] = spec;
                break;
            }

    Call *call = &sim->calls[idx];
    call->id = id;
    call->arrival = sim->now;
    call->district = random_between(sim, 0, sim->district_count - 1);
    call->x = random_between(sim, 0, 9);
    call->y = random_between(sim, 0, 9);
    call->spec = specs[random_between(sim, 0, spec_count - 1)];
    call->priority = random_between(sim, 1, 3);
    call->service_duration = random_between(sim, 5, 20);
    call->max_wait = random_between(sim, 15, 40);
    call->escalation_after = random_between(sim, 4, 10);
    call->cancel_after = random_between(sim, 0, 4) == 0 ? random_between(sim, 5, 30) : 0; // ~20% of callers may hang up
    call->state = CALL_PENDING;
    call->assigned_team = NO_TEAM;
    call->dispatch_time = 0;
    call->service_start = 0;
    call->completion_time = 0;

    log_event(sim, "GENERATOR: new random call #%03d", call->id);

    return idx;
}

/**
 * @brief Plans the arrival of the next random call
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int schedule_next_random_call(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    int gap = random_between(sim, 1, 2 * sim->random_interval - 1);

    return schedule_event_checked(sim, sim->now + gap, EV_CALL_ARRIVAL, GENERATED_CALL);
}
