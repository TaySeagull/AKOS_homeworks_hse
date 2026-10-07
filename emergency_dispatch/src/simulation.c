#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "event_queue.h"
#include "event_handlers.h"
#include "output.h"
#include "utils.h"
#include "utils_extra.h"
#include <signal.h>

#define SNAPSHOT_INTERVAL 4 // Interval in minutes for showing simulation snapshots

// sig_atomic_t is a type for working with signals 
static volatile sig_atomic_t stop_requested = 0; // Flag to indicate if a stop has been requested

/**
 * @brief Signal to request stopping the simulation
 * @param number Signal number
 */
static void on_sigint(int number)
{
    (void)number;
    // We don't use the signal number, it is just need because of the standart
    stop_requested = 1;
}

/**
 * @brief Marks a call as unserved
 * @param call Pointer to the call structure
 * @return 1 on success, 0 on failure
 */
static int mark_call_unserved(Call *call)
{
    SOFT_ASSERT(call, "call is NULL", 0);

    if (call->state == CALL_UNSERVED)
        return 0;

    call->state = CALL_UNSERVED;

    return 1;
}

/**
 * @brief Schedules a simulation event with bounds checking
 * @param sim Pointer to the simulation structure
 * @param time The time at which the event occurs
 * @param type The type of the event
 * @param id The ID of the event
 * @return 1 on success, 0 on failure
 */
int schedule_event_checked(Simulation *sim, int time, EventType type, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(push_event(sim, time, type, id), "failed to add to queue simulation event", 0);
    
    return 1;
}

/**
 * @brief Cancels or unserves expired calls
 * @param s Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int cancel_or_unserve_expired(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        Call *call = &sim->calls[idx];

        if (call->state != CALL_WAITING)
            continue;
        
        int waited = sim->now - call->arrival;

        // Check if the call has exceeded its cancel_after time
        if (call->cancel_after > 0 && waited >= call->cancel_after)
        {
            call->state = CALL_CANCELLED;
            sim->cancelled++;
            log_event(sim, "CALL #%03d CANCELLED after %d min of waiting", call->id, waited);
        }
        else if (waited >= call->max_wait)
        {
            call->state = CALL_UNSERVED;
            sim->unserved++;
            log_event(sim, "CALL #%03d UNSERVED: maximum waiting time %d min exceeded", call->id, call->max_wait);
        }
    }

    return 1;
}

/**
 * @brief Seeds the simulation with events
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int seed_events(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    for (int idx = 0; idx < sim->call_count; idx++)
        schedule_event_checked(sim, sim->calls[idx].arrival, EV_CALL_ARRIVAL, sim->calls[idx].id);
    
    return 1;
}

/**
 * @brief Marks active calls as unserved
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
static int mark_active_calls_unserved(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        Call *call = &sim->calls[idx];

        if (call->state == CALL_WAITING || call->state == CALL_EN_ROUTE || call->state == CALL_IN_SERVICE)
            if (mark_call_unserved(call))
                sim->unserved++;
    }

    return 1;
}

/**
 * @brief Runs the simulation
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int run_simulation(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    print_header(sim);

    log_event(sim, "SHIFT STARTED: districts=%d teams=%d calls=%d", sim->district_count, sim->team_count, sim->call_count);
    
    seed_events(sim);

    while (sim->event_count > 0 && !stop_requested)
    {
        Event event = pop_event(sim);

        // If the event time exceeds the shift limit, we stop processing further events
        if (event.time > sim->shift_limit)
            break;
        
        sim->now = event.time;

        cancel_or_unserve_expired(sim);

        handle_event(sim, event);

        // Show a snapshot every SNAPSHOT_INTERVAL minutes
        if (sim->completed_events % SNAPSHOT_INTERVAL == 0)
            show_snapshot(sim);
            
        sim->completed_events++;
    }

    if (stop_requested)
    {
        log_event(sim, "SIMULATION INTERRUPTED BY USER");
        mark_active_calls_unserved(sim);
    }

    sim->now = sim->shift_limit;

    cancel_or_unserve_expired(sim);

    mark_active_calls_unserved(sim);

    log_event(sim, "SHIFT FINISHED");

    print_summary(sim);

    return 1;
}

/**
 * @brief Installs the signal for SIGINT
 * @return 1 on success, 0 on failure
 */
int install_signal(void)
{
    signal(SIGINT, on_sigint); 
    return 1;
}
