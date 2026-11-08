#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "event_queue.h"
#include "generator.h"
#include "output.h"
#include "threads.h"
#include "utils.h"
#include "utils_extra.h"
#include <string.h>

#define SNAPSHOT_EVERY_STEPS 4

/**
 * @brief Schedules a simulation event with bounds checking (world mutex must be locked)
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
 * @brief Reads (and resets) the requests of the signal thread
 */
static void read_control(Simulation *sim, int *stop, int *close, int *snapshot)
{
    pthread_mutex_lock(&sim->control);
    *stop = sim->stop_requested;
    *close = sim->close_requested;
    *snapshot = sim->snapshot_requested;
    sim->snapshot_requested = 0;
    pthread_mutex_unlock(&sim->control);
}

/**
 * @brief Prints the live status box as one block (world mutex locked)
 */
static void snapshot_locked(Simulation *sim)
{
    output_begin(sim);
    show_snapshot(sim);
    output_end(sim);
}

/**
 * @brief Creates a random call and starts its thread (world mutex locked)
 * @return Id of the new call, or -1 if it could not be created
 */
static int spawn_random_call(Simulation *sim)
{
    int idx = create_random_call(sim);

    schedule_next_random_call(sim);

    if (idx < 0)
        return -1;

    if (!start_call_thread(sim, idx))
    {
        sim->calls[idx].state = CALL_UNSERVED;
        sim->received++;
        sim->unserved++;
        log_event(sim, "CALL #%03d REJECTED: no thread could be created, recorded as unserved", sim->calls[idx].id);
        return -1;
    }

    return sim->calls[idx].id;
}

/**
 * @brief Delivers one event to the thread that owns it (no mutex held)
 * @return 1 if a message was sent
 */
static int deliver(Simulation *sim, const Event *event)
{
    if (event->type == EV_TEAM_ARRIVAL || event->type == EV_SERVICE_DONE || event->type == EV_RETURN_DONE)
    {
        pthread_mutex_lock(&sim->world);
        int idx = find_team_index_by_id(sim, event->id);
        pthread_mutex_unlock(&sim->world);

        if (idx < 0)
            return 0;

        send_message(sim, &sim->teams[idx].mailbox, MSG_WAKE, event->type, idx);
        return 1;
    }

    // events of a call; a closed call ignores it, but its thread still reads the box
    pthread_mutex_lock(&sim->world);
    int idx = find_call_index_by_id(sim, event->id);
    int alive = idx >= 0 && sim->calls[idx].started;
    pthread_mutex_unlock(&sim->world);

    if (!alive)
        return 0;

    send_message(sim, &sim->calls[idx].mailbox, MSG_WAKE, event->type, idx);
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

    set_thread_tag("CLK");

    pthread_mutex_lock(&sim->world);
    output_begin(sim);
    print_header(sim);
    output_end(sim);
    log_event(sim, "SHIFT STARTED: districts=%d teams=%d calls=%d%s; threads: clock, dispatcher, signals, %d teams, %d calls",
              sim->district_count, sim->team_count, sim->call_count, sim->random_calls ? " + random generator" : "",
              sim->team_count, sim->call_count);
    int seeded = seed_events(sim);
    pthread_mutex_unlock(&sim->world);

    SOFT_ASSERT(seeded, "cannot seed initial events", 0);

    sim->stop_reason = STOP_ALL_PROCESSED;
    int steps = 0;

    for (;;)
    {
        int stop = 0, close = 0, snapshot = 0;
        read_control(sim, &stop, &close, &snapshot);

        pthread_mutex_lock(&sim->world);

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
            pthread_mutex_unlock(&sim->world);
            break;
        }

        if (sim->event_count == 0)
        {
            sim->stop_reason = sim->intake_closed ? STOP_INTAKE_CLOSED : STOP_ALL_PROCESSED;
            pthread_mutex_unlock(&sim->world);
            break;
        }

        // the shift is over if the nearest event is after its end (0 = unlimited shift)
        if (sim->shift_limit > 0 && sim->events[0].time > sim->shift_limit)
        {
            sim->stop_reason = STOP_SHIFT_LIMIT;
            sim->now = sim->shift_limit;
            pthread_mutex_unlock(&sim->world);
            break;
        }

        // ---- take all events of the next model minute ----
        sim->now = sim->events[0].time;
        int count = 0;

        while (sim->event_count > 0 && sim->events[0].time == sim->now && count < MAX_EVENTS)
        {
            batch[count] = pop_event(sim);

            // the random generator creates the call (and its thread) right now
            if (batch[count].type == EV_CALL_ARRIVAL && batch[count].id == GENERATED_CALL)
            {
                int id = sim->intake_closed ? -1 : spawn_random_call(sim);

                if (id < 0)
                    continue;

                batch[count].id = id;
            }

            count++;
        }

        pthread_mutex_unlock(&sim->world);

        // wake the owners of the events
        quiet_add(&sim->quiet);
        for (int idx = 0; idx < count; idx++)
            deliver(sim, &batch[idx]);
        quiet_done(&sim->quiet);
        quiet_wait(&sim->quiet);

        // the dispatcher assigns teams, the teams depart
        quiet_add(&sim->quiet);
        send_message(sim, &sim->dispatcher_box, MSG_ROUND, 0, 0);
        quiet_done(&sim->quiet);
        quiet_wait(&sim->quiet);

        // check the model
        pthread_mutex_lock(&sim->world);
        check_invariants(sim);
        sim->completed_events += count;
        steps++;

        if (steps % SNAPSHOT_EVERY_STEPS == 0)
            snapshot_locked(sim);

        pthread_mutex_unlock(&sim->world);

        sleep_visual(sim);
    }

    // the end of the simulation
    pthread_mutex_lock(&sim->world);

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

    pthread_mutex_unlock(&sim->world);

    return 1;
}
