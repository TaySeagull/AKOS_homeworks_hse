#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "event_queue.h"
#include "generator.h"
#include "output.h"
#include "ipc.h"
#include "signals.h"
#include "utils.h"
#include "utils_extra.h"
#include <signal.h>
#include <string.h>

#define SNAPSHOT_EVERY_STEPS 4

/**
 * @brief Schedules a simulation event with bounds checking (world mutex must be locked)
 * @param sim Pointer to the simulation structure
 * @param time The time of the event
 * @param type The type of the event
 * @param id The ID of the event
 * @return 1 on success, 0 on failure
 */
int schedule_event_checked(Simulation *sim, int time, EventType type, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(time > sim->now, "a new event must be later than the current minute", 0);
    SOFT_ASSERT(push_event(sim, time, type, id), "failed to add to queue simulation event", 0);

    return 1;
}

/**
 * @brief Puts the arrivals of all configured calls into the queue
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int seed_events(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    for (int idx = 0; idx < sim->call_count; idx++)
        SOFT_ASSERT(push_event(sim, sim->calls[idx].arrival, EV_CALL_ARRIVAL, sim->calls[idx].id),
                    "cannot schedule call arrival", 0);

    if (sim->random_calls)
    {
        sim->next_call_id = 1;
        for (int idx = 0; idx < sim->call_count; idx++)
            if (sim->calls[idx].id >= sim->next_call_id)
                sim->next_call_id = sim->calls[idx].id + 1;

        sim->rng = sim->seed;
        int first = (int)(rand_r(&sim->rng) % (unsigned)sim->random_interval);

        SOFT_ASSERT(push_event(sim, first, EV_CALL_ARRIVAL, GENERATED_CALL), "cannot schedule generated call", 0);
    }

    return 1;
}

/**
 * @brief Checks the model invariants from the task statement (world mutex must be locked)
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
 * @brief Fixes all calls that are still active when the simulation stops (world mutex locked)
 * @param sim Pointer to the simulation structure
 * @param reason The reason for marking the calls as unserved
 */
static void mark_active_calls_unserved(Simulation *sim, const char *reason)
{
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
}

/**
 * @brief Prints the live status box as one block (world mutex locked)
 * @param sim Pointer to the simulation structure
 */
static void snapshot_locked(Simulation *sim)
{
    output_begin(sim);
    show_snapshot(sim);
    output_end(sim);
}

/**
 * @brief Creates a random call and starts its process (world lock is held)
 * @param sim Pointer to the simulation structure
 * @return Id of the new call, or -1 if it could not be created
 */
static int spawn_random_call(Simulation *sim)
{
    int idx = create_random_call(sim);

    schedule_next_random_call(sim);

    if (idx < 0)
        return -1;

    if (!ipc_start_call(sim, idx))
    {
        sim->calls[idx].state = CALL_UNSERVED;
        sim->received++;
        sim->unserved++;
        log_event(sim, "CALL #%03d REJECTED: no process could be created, recorded as unserved", sim->calls[idx].id);
        return -1;
    }

    return sim->calls[idx].id;
}

/**
 * @brief Delivers one event to the process that owns it (world lock is not held)
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event to be delivered
 * @return 1 if a message was sent
 */
static int deliver(Simulation *sim, const Event *event)
{
    if (event->type == EV_TEAM_ARRIVAL || event->type == EV_SERVICE_DONE || event->type == EV_RETURN_DONE)
    {
        world_lock(sim);
        int idx = find_team_index_by_id(sim, event->id);
        world_unlock(sim);

        if (idx < 0)
            return 0;

        ipc_send_simple(sim, BOX_TEAM, idx, MSG_WAKE, event->type, idx, 0);
        return 1;
    }

    // events of a call; a closed call ignores it, but its process still reads the box
    world_lock(sim);
    int idx = find_call_index_by_id(sim, event->id);
    int alive = idx >= 0 && sim->calls[idx].started;
    world_unlock(sim);

    if (!alive)
        return 0;

    ipc_send_simple(sim, BOX_CALL, idx, MSG_WAKE, event->type, idx, 0);
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

    static Event batch[MAX_EVENTS]; // events of one model minute

    set_process_tag("CLK");

    world_lock(sim);
    output_begin(sim);
    print_header(sim);
    output_end(sim);
    log_event(sim, "SHIFT STARTED: districts=%d teams=%d calls=%d%s; processes: clock, dispatcher, %d teams, %s",
              sim->district_count, sim->team_count, sim->call_count, sim->random_calls ? " + random generator" : "",
              sim->team_count, ipc_calls_layout());
    int seeded = seed_events(sim);
    world_unlock(sim);

    SOFT_ASSERT(seeded, "cannot seed initial events", 0);

    sim->stop_reason = STOP_ALL_PROCESSED;
    int steps = 0;
    int reported_stop = 0, reported_close = 0;

    for (;;)
    {
        // requests of the user received by the signal handlers
        ControlRequests control;
        signals_take(&control);

        world_lock(sim);

        if (control.stop && !reported_stop)
        {
            log_event(sim, "SIGNAL %s", signal_text(control.stop_signal));
            reported_stop = 1;
        }

        if (control.close && !reported_close)
        {
            log_event(sim, "SIGNAL %s", signal_text(SIGQUIT));
            reported_close = 1;
        }

        if (control.snapshot)
            log_event(sim, "SIGNAL %s", signal_text(SIGUSR1));

        int stop = control.stop, close = control.close, snapshot = control.snapshot;

        if (close && !sim->intake_closed)
        {
            sim->intake_closed = 1;
            sim->random_calls = 0;
            int removed = remove_call_arrivals(sim);
            log_event(sim, "INTAKE CLOSED BY USER: %d future call(s) will not be accepted, active calls are finished", removed);
        }

        if (snapshot)
            snapshot_locked(sim);

        if (stop)
        {
            sim->stop_reason = STOP_INTERRUPTED;
            world_unlock(sim);
            break;
        }

        if (sim->event_count == 0)
        {
            sim->stop_reason = sim->intake_closed ? STOP_INTAKE_CLOSED : STOP_ALL_PROCESSED;
            world_unlock(sim);
            break;
        }

        // the shift is over if the nearest event is after its end (0 = unlimited shift)
        if (sim->shift_limit > 0 && sim->events[0].time > sim->shift_limit)
        {
            sim->stop_reason = STOP_SHIFT_LIMIT;
            sim->now = sim->shift_limit;
            world_unlock(sim);
            break;
        }

        // ---- take all events of the next model minute ----
        sim->now = sim->events[0].time;
        int count = 0;

        while (sim->event_count > 0 && sim->events[0].time == sim->now && count < MAX_EVENTS)
        {
            batch[count] = pop_event(sim);

            // the random generator creates the call (and its process) right now
            if (batch[count].type == EV_CALL_ARRIVAL && batch[count].id == GENERATED_CALL)
            {
                int id = sim->intake_closed ? -1 : spawn_random_call(sim);

                if (id < 0)
                    continue;

                batch[count].id = id;
            }

            count++;
        }

        world_unlock(sim);

        // wake the owners of the events
        ipc_quiet_hold(sim);
        for (int idx = 0; idx < count; idx++)
            deliver(sim, &batch[idx]);
        ipc_quiet_release(sim);

        if (!ipc_quiet_wait(sim))
        {
            sim->stop_reason = STOP_PROCESS_FAILURE;
            break;
        }

        // the dispatcher assigns teams, the teams depart
        ipc_quiet_hold(sim);
        ipc_send_simple(sim, BOX_DISPATCHER, 0, MSG_ROUND, 0, 0, 0);
        ipc_quiet_release(sim);

        if (!ipc_quiet_wait(sim))
        {
            sim->stop_reason = STOP_PROCESS_FAILURE;
            break;
        }

        // check the model
        world_lock(sim);
        check_invariants(sim);
        sim->completed_events += count;
        steps++;

        if (steps % SNAPSHOT_EVERY_STEPS == 0)
            snapshot_locked(sim);

        world_unlock(sim);

        sleep_visual(sim);
    }

    // the end of the simulation
    world_lock(sim);

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

    case STOP_INTAKE_CLOSED:
        log_event(sim, "ALL ACCEPTED CALLS PROCESSED (intake was closed)");
        break;

    case STOP_PROCESS_FAILURE:
        log_event(sim, "SIMULATION ABORTED: a process of the simulation terminated unexpectedly");
        mark_active_calls_unserved(sim, "a process of the simulation has died");
        break;

    case STOP_ALL_PROCESSED:
    default:
        log_event(sim, "ALL CALLS PROCESSED");
        break;
    }

    int not_arrived = 0;
    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].state == CALL_PENDING)
            not_arrived++;

    if (not_arrived > 0)
        log_event(sim, "%d call(s) were scheduled after the end of the simulation and never arrived", not_arrived);

    log_event(sim, "SHIFT FINISHED");

    snapshot_locked(sim);

    world_unlock(sim);

    return 1;
}
