#define _POSIX_C_SOURCE 200809L

#include "roles.h"
#include "dispatch.h"
#include "ipc.h"
#include "simulation.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Reaction of a call to one of its own events (world lock is held)
 * @param sim Pointer to the simulation structure
 * @param call The call
 * @param event Type of the event
 * @param notify Out: message for the dispatcher (-1 = none)
 * @return 1 if the record of the call has been changed
 */
static int call_on_event(Simulation *sim, Call *call, int event, int *notify)
{
    *notify = -1;

    switch (event)
    {
    case EV_CALL_ARRIVAL:
        if (call->state != CALL_PENDING)
            return 0;

        call->state = CALL_WAITING;
        sim->received++;

        log_event(sim, "CALL #%03d ARRIVED: district=%s (x=%d, y=%d) priority=%d spec=%s service=%d min max_wait=%d min",
                  call->id, sim->districts[call->district].name, call->x, call->y, call->priority,
                  spec_name(call->spec), call->service_duration, call->max_wait);

        // personal deadlines of the caller
        if (sim->escalation_enabled && call->escalation_after > 0 && call->priority > 1)
            ipc_schedule(sim, call->arrival + call->escalation_after, EV_ESCALATION, call->id);

        if (call->cancel_after > 0)
            ipc_schedule(sim, call->arrival + call->cancel_after, EV_CANCEL, call->id);

        ipc_schedule(sim, call->arrival + call->max_wait, EV_WAIT_TIMEOUT, call->id);

        *notify = MSG_NEW_CALL;
        return 1;

    case EV_ESCALATION:
        if (call->state != CALL_WAITING || call->priority <= 1)
            return 0;

        call->priority--;
        sim->escalations++;

        log_event(sim, "CALL #%03d PRIORITY ESCALATED %d -> %d after %d min of waiting", call->id,
                  call->priority + 1, call->priority, sim->now - call->arrival);

        if (call->priority > 1)
            ipc_schedule(sim, sim->now + call->escalation_after, EV_ESCALATION, call->id);

        *notify = MSG_CALL_CHANGED;
        return 1;

    case EV_CANCEL:
        if (call->state != CALL_WAITING)
            return 0;

        call->state = CALL_CANCELLED;
        ipc_remove_deadlines(sim, call->id);
        sim->cancelled++;

        log_event(sim, "CALL #%03d CANCELLED by the caller after %d min of waiting", call->id, call->cancel_after);
        return 1;

    case EV_WAIT_TIMEOUT:
        if (call->state != CALL_WAITING)
            return 0;

        call->state = CALL_UNSERVED;
        ipc_remove_deadlines(sim, call->id);
        sim->unserved++;

        log_event(sim, "CALL #%03d UNSERVED: maximum waiting time %d min exceeded", call->id, call->max_wait);
        return 1;

    default:
        log_event(sim, "ERROR: CALL #%03d got an unexpected event %d", call->id, event);
        return 0;
    }
}

/**
 * @brief Handles one message addressed to a call
 * @param sim Pointer to the simulation structure
 * @param call_idx Index of the call
 * @param message The message
 */
static void call_handle(Simulation *sim, int call_idx, const Message *message)
{
    if (call_idx < 0 || call_idx >= MAX_CALLS)
        return;

    Call *call = &sim->calls[call_idx];

    char tag[16];
    snprintf(tag, sizeof(tag), "C%03d", call->id);
    set_process_tag(tag); // the family process speaks for the call it handles now

    if (message->type != MSG_WAKE)
        return; // MSG_CALL_FINISHED / MSG_SYNC: nothing else to do for the caller

    int notify = -1;

    world_lock(sim);
    int changed = call_on_event(sim, call, message->event, &notify); // a closed call ignores late wake-ups
    world_unlock(sim);

    if (changed)
        ipc_publish_call(sim, call_idx);

    // the dispatcher gets a message only after the lock is released
    if (notify >= 0)
        ipc_send_simple(sim, BOX_DISPATCHER, 0, (MsgType)notify, 0, call_idx, 0);
}

/**
 * @brief Handles one message addressed to a team
 * @param sim Pointer to the simulation structure
 * @param team_idx Index of the team
 * @param message The message
 */
static void team_handle(Simulation *sim, int team_idx, const Message *message)
{
    Team *team = &sim->teams[team_idx];

    int finished_call = -1; // index of the call that must be told "help finished"
    int touched_call = -1;  // index of the call whose record was changed
    int became_free = 0;
    int changed = 0;

    world_lock(sim);

    if (message->type == MSG_ASSIGN)
    {
        // the dispatcher has already reserved the team (state EN_ROUTE)
        Call *call = &sim->calls[message->arg];
        int time = travel_time(sim, team, call->district);

        log_event(sim, "B%d DEPARTED from %s to %s for CALL #%03d, travel TIME=%d min", team->id,
                  sim->districts[team->district].name, sim->districts[call->district].name, call->id, time);

        ipc_schedule(sim, sim->now + time, EV_TEAM_ARRIVAL, team->id);
    }
    else if (message->type == MSG_WAKE && message->event == EV_TEAM_ARRIVAL)
    {
        int call_idx = find_call_index_by_id(sim, team->call_id);

        if (team->state == TEAM_EN_ROUTE && call_idx >= 0 && sim->calls[call_idx].state == CALL_EN_ROUTE)
        {
            Call *call = &sim->calls[call_idx];

            team->district = call->district;
            team->state = TEAM_AT_SCENE;
            call->state = CALL_IN_SERVICE;
            call->service_start = sim->now;
            changed = 1;
            touched_call = call_idx;

            log_event(sim, "B%d ARRIVED at %s for CALL #%03d (after %d min of waiting)", team->id,
                      sim->districts[team->district].name, call->id, sim->now - call->arrival);
            log_event(sim, "B%d STARTED HELP on CALL #%03d, it will take %d min", team->id, call->id,
                      call->service_duration);

            ipc_schedule(sim, sim->now + call->service_duration, EV_SERVICE_DONE, team->id);
        }
        else
            log_event(sim, "ERROR: B%d arrived in a wrong state", team->id);
    }
    else if (message->type == MSG_WAKE && message->event == EV_SERVICE_DONE)
    {
        int call_idx = find_call_index_by_id(sim, team->call_id);

        if (team->state == TEAM_AT_SCENE && call_idx >= 0)
        {
            Call *call = &sim->calls[call_idx];

            call->state = CALL_COMPLETED;
            call->completion_time = sim->now;

            sim->served++;
            sim->total_wait += call->service_start - call->arrival;
            sim->total_service += call->completion_time - call->service_start;

            log_event(sim, "CALL #%03d HELP FINISHED by B%d; total wait=%d min, service=%d min", call->id, team->id,
                      call->service_start - call->arrival, call->completion_time - call->service_start);

            team->call_id = NO_CALL;
            finished_call = call_idx;
            touched_call = call_idx;
            changed = 1;

            int time = -1;
            if (sim->return_to_base && team->district != team->base)
                time = travel_time(sim, team, team->base);

            if (time > 0)
            {
                team->state = TEAM_RETURNING;
                log_event(sim, "B%d RELEASED; returning to base %s, TIME=%d min", team->id,
                          sim->districts[team->base].name, time);
                ipc_schedule(sim, sim->now + time, EV_RETURN_DONE, team->id);
            }
            else
            {
                team->state = TEAM_AVAILABLE;
                became_free = 1;

                if (sim->return_to_base && team->district != team->base)
                    log_event(sim, "B%d RELEASED: no road to base, AVAILABLE in district %s", team->id,
                              sim->districts[team->district].name);
                else
                    log_event(sim, "B%d RELEASED and AVAILABLE in district %s", team->id,
                              sim->districts[team->district].name);
            }
        }
        else
            log_event(sim, "ERROR: B%d finished service in a wrong state", team->id);
    }
    else if (message->type == MSG_WAKE && message->event == EV_RETURN_DONE)
    {
        if (team->state == TEAM_RETURNING)
        {
            team->district = team->base;
            team->state = TEAM_AVAILABLE;
            became_free = 1;
            changed = 1;

            log_event(sim, "B%d RETURNED to base %s and is AVAILABLE", team->id, sim->districts[team->base].name);
        }
        else
            log_event(sim, "ERROR: B%d returned in a wrong state", team->id);
    }

    world_unlock(sim);

    // messages are sent only after the lock is released
    if (changed)
        ipc_publish_team(sim, team_idx);

    if (touched_call >= 0)
        ipc_publish_call(sim, touched_call);

    if (finished_call >= 0)
        ipc_send_simple(sim, BOX_CALL, finished_call, MSG_CALL_FINISHED, 0, finished_call, team_idx);

    if (became_free)
        ipc_send_simple(sim, BOX_DISPATCHER, 0, MSG_TEAM_FREE, 0, team_idx, 0);
}

/**
 * @struct DispatcherState
 * @brief Private data of the dispatcher process between messages
 */
typedef struct
{
    int dirty;               // something has changed since the last round
    int new_calls[MAX_CALLS];
    int new_count;           // calls that came during the current minute
} DispatcherState;

/**
 * @brief Handles one message addressed to the dispatcher
 * @param sim Pointer to the simulation structure
 * @param state Private state of the dispatcher
 * @param message The message
 */
static void dispatcher_handle(Simulation *sim, DispatcherState *state, const Message *message)
{
    switch (message->type)
    {
    case MSG_NEW_CALL:
        if (state->new_count < MAX_CALLS)
            state->new_calls[state->new_count++] = message->arg;
        state->dirty = 1;
        break;

    case MSG_CALL_CHANGED:
    case MSG_TEAM_FREE:
        state->dirty = 1;
        break;

    case MSG_ROUND:
    {
        Assignment plan[MAX_TEAMS];
        Message orders[MAX_TEAMS];
        int made = 0;

        if (state->dirty)
        {
            world_lock(sim);

            made = plan_dispatch(sim, plan, MAX_TEAMS);

            for (int idx = 0; idx < state->new_count; idx++)
            {
                const Call *call = &sim->calls[state->new_calls[idx]];

                if (call->state == CALL_WAITING)
                {
                    int waiting = 0;
                    for (int jdx = 0; jdx < sim->call_count; jdx++)
                        if (sim->calls[jdx].state == CALL_WAITING)
                            waiting++;

                    log_event(sim, "SEARCH: no free %s team can reach %s; CALL #%03d is queued (waiting calls: %d)",
                              spec_name(call->spec), sim->districts[call->district].name, call->id, waiting);
                }
            }

            // the order carries a copy of the call (needed by the message-queue version)
            for (int idx = 0; idx < made; idx++)
            {
                memset(&orders[idx], 0, sizeof(orders[idx]));
                orders[idx].type = MSG_ASSIGN;
                orders[idx].arg = plan[idx].call;
                orders[idx].call_idx = plan[idx].call;
                orders[idx].call = sim->calls[plan[idx].call];
                orders[idx].team_idx = -1;
            }

            world_unlock(sim);
        }

        // the teams start moving by themselves after the order
        for (int idx = 0; idx < made; idx++)
        {
            ipc_publish_call(sim, plan[idx].call);
            ipc_publish_team(sim, plan[idx].team);
            ipc_send(sim, BOX_TEAM, plan[idx].team, &orders[idx]);
        }

        state->dirty = 0;
        state->new_count = 0;
        break;
    }

    default:
        break;
    }
}


/**
 * @brief Main loop of a dispatcher, team, call or callers process
 * @param sim Pointer to the simulation structure
 * @param role Role of the process
 * @param idx Index of the team / call (ignored for the dispatcher and callers)
 * @return 1 on normal end
 */
int role_loop(Simulation *sim, Role role, int idx)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    char tag[16];

    switch (role)
    {
    case ROLE_DISPATCHER:
        snprintf(tag, sizeof(tag), "DSP");
        break;
    case ROLE_TEAM:
        snprintf(tag, sizeof(tag), "B%d", sim->teams[idx].id);
        break;
    case ROLE_CALL:
        snprintf(tag, sizeof(tag), "C%03d", sim->calls[idx].id);
        break;
    default:
        snprintf(tag, sizeof(tag), "CLR");
        break;
    }

    set_process_tag(tag);

    DispatcherState dispatcher;
    memset(&dispatcher, 0, sizeof(dispatcher));

    for (;;)
    {
        Message message = ipc_receive(sim);

        if (message.type == MSG_SHUTDOWN) // not counted as work
            break;

        ipc_apply(sim, &message); // message-queue version: refresh the mirror

        switch (role)
        {
        case ROLE_DISPATCHER:
            dispatcher_handle(sim, &dispatcher, &message);
            break;

        case ROLE_TEAM:
            if (message.type != MSG_SYNC)
                team_handle(sim, idx, &message);
            break;

        case ROLE_CALL:
            call_handle(sim, idx, &message);
            break;

        case ROLE_CALLERS:
            if (message.type == MSG_WAKE || message.type == MSG_CALL_FINISHED)
                call_handle(sim, message.arg, &message);
            set_process_tag(tag);
            break;

        default:
            break;
        }

        ipc_done(sim); // must be the last action of the reaction
    }

    return 1;
}
