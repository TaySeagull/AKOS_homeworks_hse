#include "dispatch.h"
#include "utils.h"
#include "utils_extra.h"
#include "simulation.h"
#include <limits.h>

#define BASE_SPEED 30.0

/**
 * @brief Calculates the travel time between a team and a target district
 * @param sim Pointer to the simulation structure
 * @param team Pointer to the team structure
 * @param target The target district
 * @return The travel time, or -1 if invalid
 */
int travel_time(const Simulation *sim, const Team *team, int target)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);
    SOFT_ASSERT(team, "team is NULL", -1);

    SOFT_ASSERT(team->district >= 0 && team->district < sim->district_count, "team district is out of range", -1);

    SOFT_ASSERT(target >= 0 && target < sim->district_count, "target district is out of range", -1);

    SOFT_ASSERT(team->speed > 0, "team speed must be positive", -1);

    int base_time = sim->districts[team->district].travel[target];

    // invalid travel time, no path between districts
    if (base_time < 0)
        return -1;

    double how_much_faster = BASE_SPEED / (double)team->speed;

    // 0.5 is added for rounding to the nearest integer
    int result = (int)(base_time * how_much_faster + 0.5);

    return result < 1 ? 1 : result;
}

/**
 * @brief Checks if a team can serve a call
 * @param team Pointer to the team structure
 * @param call Pointer to the call structure
 * @return 1 if the team can serve the call, 0 otherwise
 */
int team_can_serve(const Team *team, const Call *call)
{
    SOFT_ASSERT(team, "team is NULL", 0);
    SOFT_ASSERT(call, "call is NULL", 0);

    return team->state == TEAM_AVAILABLE && (int)team->spec == call->spec;
}

/**
 * @brief Chooses the best available team for a call based on the simulation strategy
 * @param sim Pointer to the simulation structure
 * @param call Pointer to the call structure
 * @return The index of the chosen team, or -1 if no suitable team is found
 */
int choose_team(Simulation *sim, const Call *call)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);
    SOFT_ASSERT(call, "call is NULL", -1);

    int best = -1;
    int best_time = INT_MAX, best_dist = INT_MAX; // have to be bigger than any possible travel time or distance

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        Team *team = &sim->teams[idx];

        if (!team_can_serve(team, call))
            continue;

        int time = travel_time(sim, team, call->district);

        // team cannot reach the call district
        if (time < 0)
            continue;

        int dist = sim->districts[team->district].travel[call->district];

        // Choose the best team based on the strategy
        if (sim->strategy == STRATEGY_FASTEST)
        {
            if (time < best_time || (time == best_time && team->id < sim->teams[best].id))
            {
                best = idx;
                best_time = time;
            }
        }
        // nearest strategy
        else if (dist < best_dist || (dist == best_dist && time < best_time) || (dist == best_dist && time == best_time && team->id < sim->teams[best].id))
        {
            best = idx;
            best_dist = dist;
            best_time = time;
        }
    }

    return best;
}

/**
 * @brief Tries to dispatch calls to available teams
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int try_dispatch(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    for (;;)
    {
        int selected = -1, priority = 99, wait_best = -1;

        for (int idx = 0; idx < sim->call_count; idx++)
        {
            Call *call = &sim->calls[idx];

            if (call->state != CALL_WAITING)
                continue;

            if (choose_team(sim, call) < 0)
                continue;

            int wait = sim->now - call->arrival;

            // Select the call with the highest priority and longest waiting time
            if (call->priority < priority || (call->priority == priority && wait > wait_best))
            {
                selected = idx;
                priority = call->priority;
                wait_best = wait;
            }
        }

        if (selected < 0)
            break;

        Call *call_ = &sim->calls[selected];

        int chosen_team = choose_team(sim, call_);
        SOFT_ASSERT(chosen_team >= 0 && chosen_team < sim->team_count, "selected team index is invalid", 0);

        Team *team = &sim->teams[chosen_team];

        int time = travel_time(sim, team, call_->district);
        SOFT_ASSERT(time >= 0, "team cannot reach call district", 0);

        call_->assigned_team = team->id;
        call_->dispatch_time = sim->now;
        call_->state = CALL_EN_ROUTE;
        team->state = TEAM_EN_ROUTE;
        team->call_id = call_->id;

        sim->dispatches++;

        log_event(sim, "DISPATCH: CALL #%03d priority=%d spec=%s -> B%d (%s), TIME=%d min", call_->id, call_->priority, spec_name(call_->spec), team->id, strategy_name(sim->strategy), time);

        if (!schedule_event_checked(sim, sim->now + time, EV_TEAM_ARRIVAL, team->id))
            return 0;

        sleep_visual(sim);
    }

    return 1;
}