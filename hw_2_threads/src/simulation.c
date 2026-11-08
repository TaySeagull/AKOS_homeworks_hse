#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "event_queue.h"
#include "event_handlers.h"
#include "output.h"
#include "utils.h"
#include "utils_extra.h"
#include <signal.h>
#include <string.h>

#define SNAPSHOT_EVERY_EVENTS 4 // a live status box is shown after every 4 processed events

// sig_atomic_t is the only type that can be safely written from a signal handler
static volatile sig_atomic_t stop_requested = 0;     // SIGINT / SIGTERM: stop immediately
static volatile sig_atomic_t close_requested = 0;    // SIGQUIT: do not accept new calls
static volatile sig_atomic_t snapshot_requested = 0; // SIGUSR1: print the live status

/**
 * @brief Signal handler: only sets flags, the main loop reacts to them
 * @param number Signal number
 */
static void on_signal(int number)
{
    if (number == SIGINT || number == SIGTERM)
        stop_requested = 1;
    else if (number == SIGQUIT)
        close_requested = 1;
    else if (number == SIGUSR1)
        snapshot_requested = 1;
}

/**
 * @brief Reacts to SIGQUIT and SIGUSR1 between two events
 * @param sim Pointer to the simulation structure
 */
static void process_signal_requests(Simulation *sim)
{
    if (close_requested && !sim->intake_closed)
    {
        sim->intake_closed = 1;
        sim->random_calls = 0; // the generator is stopped as well
        int removed = remove_call_arrivals(sim);
        log_event(sim, "INTAKE CLOSED BY USER: %d future call(s) will not be accepted, active calls are finished", removed);
    }

    if (snapshot_requested)
    {
        snapshot_requested = 0;
        show_snapshot(sim);
    }
}

/**
 * @brief Returns 1 if the user requested to stop the simulation
 */
int stop_is_requested(void)
{
    return stop_requested != 0;
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
 * @param sim Pointer to the simulation structure
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
            remove_call_deadlines(sim, call->id);
            sim->cancelled++;
            log_event(sim, "CALL #%03d CANCELLED by the caller after %d min of waiting", call->id, waited);
        }
        else if (waited >= call->max_wait)
        {
            call->state = CALL_UNSERVED;
            remove_call_deadlines(sim, call->id);
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
        SOFT_ASSERT(schedule_event_checked(sim, sim->calls[idx].arrival, EV_CALL_ARRIVAL, sim->calls[idx].id),
                    "cannot schedule call arrival", 0);

    if (sim->random_calls)
    {
        // generated ids continue after the largest configured id
        sim->next_call_id = 1;
        for (int idx = 0; idx < sim->call_count; idx++)
            if (sim->calls[idx].id >= sim->next_call_id)
                sim->next_call_id = sim->calls[idx].id + 1;

        sim->rng = sim->seed;
        int first = (int)(rand_r(&sim->rng) % (unsigned)sim->random_interval);

        SOFT_ASSERT(schedule_event_checked(sim, first, EV_CALL_ARRIVAL, GENERATED_CALL),
                    "cannot schedule generated call", 0);
    }

    return 1;
}

/**
 * @brief Checks the model invariants from the task statement\
 * Every violation is printed and counted in sim->invariant_errors.
 * @param sim Pointer to the simulation structure
 * @return 1 if all invariants hold, 0 otherwise
 */
int check_invariants(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    int ok = 1;

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        const Team *team = &sim->teams[idx];
        int busy = team->state == TEAM_EN_ROUTE || team->state == TEAM_AT_SCENE;

        if (!busy)
        {
            if (team->call_id != NO_CALL)
            {
                log_event(sim, "INVARIANT VIOLATION: B%d is %s but holds CALL #%03d", team->id, team_state_name(team->state), team->call_id);
                ok = 0;
            }
            continue;
        }

        int call_idx = find_call_index_by_id(sim, team->call_id);
        if (call_idx < 0)
        {
            log_event(sim, "INVARIANT VIOLATION: B%d is %s without a call", team->id, team_state_name(team->state));
            ok = 0;
            continue;
        }

        const Call *call = &sim->calls[call_idx];
        CallState expected = team->state == TEAM_EN_ROUTE ? CALL_EN_ROUTE : CALL_IN_SERVICE;

        if (call->state != expected || call->assigned_team != team->id || (int)team->spec != call->spec)
        {
            log_event(sim, "INVARIANT VIOLATION: B%d (%s) and CALL #%03d (%s) disagree", team->id,
                      team_state_name(team->state), call->id, call_state_name(call->state));
            ok = 0;
        }
    }

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        const Call *call = &sim->calls[idx];
        int owners = 0;

        for (int jdx = 0; jdx < sim->team_count; jdx++)
            if (sim->teams[jdx].call_id == call->id)
                owners++;

        int active = call->state == CALL_EN_ROUTE || call->state == CALL_IN_SERVICE;

        if ((active && owners != 1) || (!active && owners != 0))
        {
            log_event(sim, "INVARIANT VIOLATION: CALL #%03d (%s) is held by %d teams", call->id, call_state_name(call->state), owners);
            ok = 0;
        }
    }

    if (!ok)
        sim->invariant_errors++;

    return ok;
}

/**
 * @brief Fixes all calls that are still active when the simulation stops
 * @param sim Pointer to the simulation structure
 * @param reason Text that explains why the call stays unserved
 * @return 1 on success, 0 on failure
 */
static int mark_active_calls_unserved(Simulation *sim, const char *reason)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(reason, "reason is NULL", 0);

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        Call *call = &sim->calls[idx];

        if (call->state != CALL_WAITING && call->state != CALL_EN_ROUTE && call->state != CALL_IN_SERVICE)
            continue;

        if (call->assigned_team != NO_TEAM)
            log_event(sim, "CALL #%03d UNSERVED: %s (state %s, team B%d)", call->id, reason, call_state_name(call->state), call->assigned_team);
        else
            log_event(sim, "CALL #%03d UNSERVED: %s (state %s)", call->id, reason, call_state_name(call->state));

        call->state = CALL_UNSERVED;
        sim->unserved++;
    }

    return 1;
}

/**
 * @brief Prints the calls that never arrived (their arrival is after the stop)
 * @param sim Pointer to the simulation structure
 */
static void report_not_arrived(Simulation *sim)
{
    int count = 0;

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].state == CALL_PENDING)
            count++;

    if (count > 0)
        log_event(sim, "%d call(s) were scheduled after the end of the simulation and never arrived", count);
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

    log_event(sim, "SHIFT STARTED: districts=%d teams=%d calls=%d%s", sim->district_count, sim->team_count,
              sim->call_count, sim->random_calls ? " + random generator" : "");

    SOFT_ASSERT(seed_events(sim), "cannot seed initial events", 0);

    sim->stop_reason = STOP_ALL_PROCESSED;

    while (sim->event_count > 0 || close_requested || snapshot_requested)
    {
        process_signal_requests(sim);

        if (sim->event_count == 0)
            break;

        if (stop_requested)
        {
            sim->stop_reason = STOP_INTERRUPTED;
            break;
        }

        // the shift is over if the nearest event is after its end (0 = unlimited shift)
        if (sim->shift_limit > 0 && sim->events[0].time > sim->shift_limit)
        {
            sim->stop_reason = STOP_SHIFT_LIMIT;
            sim->now = sim->shift_limit;
            break;
        }

        Event event = pop_event(sim);

        sim->now = event.time;

        cancel_or_unserve_expired(sim);

        handle_event(sim, event);

        check_invariants(sim);

        sim->completed_events++;

        if (sim->completed_events % SNAPSHOT_EVERY_EVENTS == 0)
            show_snapshot(sim);
    }

    if (stop_requested && sim->stop_reason == STOP_ALL_PROCESSED && sim->event_count > 0)
        sim->stop_reason = STOP_INTERRUPTED;

    switch (sim->stop_reason)
    {
    case STOP_INTERRUPTED:
        log_event(sim, "SIMULATION INTERRUPTED BY USER");
        mark_active_calls_unserved(sim, "simulation was interrupted");
        break;

    case STOP_SHIFT_LIMIT:
        log_event(sim, "SHIFT LIMIT %d min REACHED", sim->shift_limit);
        mark_active_calls_unserved(sim, "the shift is over");
        break;

    case STOP_ALL_PROCESSED:
    default:
        if (sim->intake_closed)
        {
            sim->stop_reason = STOP_INTAKE_CLOSED;
            log_event(sim, "ALL ACCEPTED CALLS PROCESSED (intake was closed)");
        }
        else
            log_event(sim, "ALL CALLS PROCESSED");
        break;
    }

    report_not_arrived(sim);

    log_event(sim, "SHIFT FINISHED");

    show_snapshot(sim);

    print_summary(sim);

    return 1;
}

/**
 * @brief Installs handlers for SIGINT (Ctrl+C), SIGTERM, SIGQUIT (Ctrl+\\) and SIGUSR1
 * @return 1 on success, 0 on failure
 */
int install_signal(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    SOFT_ASSERT(sigaction(SIGINT, &action, NULL) == 0, "cannot install SIGINT handler", 0);
    SOFT_ASSERT(sigaction(SIGTERM, &action, NULL) == 0, "cannot install SIGTERM handler", 0);
    SOFT_ASSERT(sigaction(SIGQUIT, &action, NULL) == 0, "cannot install SIGQUIT handler", 0);
    SOFT_ASSERT(sigaction(SIGUSR1, &action, NULL) == 0, "cannot install SIGUSR1 handler", 0);

    return 1;
}
