#include "event_handlers.h"
#include "dispatch.h"
#include "generator.h"
#include "simulation.h"
#include "event_queue.h"
#include "output.h"
#include "utils.h"
#include "utils_extra.h"

/**
 * @brief Counts calls that are waiting for a team
 * @param sim Pointer to the simulation structure
 * @return Number of calls in state CALL_WAITING
 */
static int waiting_calls(const Simulation *sim)
{
    int count = 0;

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].state == CALL_WAITING)
            count++;

    return count;
}

/**
 * @brief Handles a call arrival event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_call_arrival(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int idx = -1;

    if (event->id == GENERATED_CALL)
    {
        // random generator: create the call now and plan the next one
        if (sim->intake_closed)
            return 1;

        idx = create_random_call(sim);
        schedule_next_random_call(sim);

        if (idx < 0)
            return 1; // the call table is full, the call was rejected and reported
    } else
        idx = find_call_index_by_id(sim, event->id);

    SOFT_ASSERT(idx >= 0, "call for arrival event not found", 0);

    Call *call = &sim->calls[idx];

    // The call must be pending before its arrival event.
    if (call->state != CALL_PENDING)
        return 1;

    // The call has arrived
    call->state = CALL_WAITING;
    sim->received++;

    log_event(
        sim,
        "CALL #%03d ARRIVED: district=%s (x=%d, y=%d) priority=%d spec=%s service=%d min max_wait=%d min",
        call->id,
        sim->districts[call->district].name,
        call->x,
        call->y,
        call->priority,
        spec_name(call->spec),
        call->service_duration,
        call->max_wait);

    // Schedule the personal deadlines of the call
    if (sim->escalation_enabled && call->escalation_after > 0 && call->priority > 1)
        schedule_event_checked(sim, call->arrival + call->escalation_after, EV_ESCALATION, call->id);

    if (call->cancel_after > 0)
        schedule_event_checked(sim, call->arrival + call->cancel_after, EV_CANCEL, call->id);

    // the call is fixed as unserved exactly when max_wait expires
    schedule_event_checked(sim, call->arrival + call->max_wait, EV_WAIT_TIMEOUT, call->id);

    try_dispatch(sim);

    if (call->state == CALL_WAITING)
        log_event(sim, "SEARCH: no free %s team can reach %s; CALL #%03d is queued (waiting calls: %d)",
                  spec_name(call->spec), sim->districts[call->district].name, call->id, waiting_calls(sim));

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a call escalation event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_escalation(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int idx = find_call_index_by_id(sim, event->id);
    if (idx < 0)
        return 1; // the call slot was reused by the random generator

    Call *call = &sim->calls[idx];
    if (call->state != CALL_WAITING || call->priority <= 1)
        return 1;

    int old_priority = call->priority;
    call->priority--;
    sim->escalations++;

    log_event(sim, "CALL #%03d PRIORITY ESCALATED %d -> %d after %d min of waiting", call->id, old_priority,
              call->priority, sim->now - call->arrival);

    // next escalation step, if the call can still become more urgent
    if (call->priority > 1)
        schedule_event_checked(sim, sim->now + call->escalation_after, EV_ESCALATION, call->id);

    try_dispatch(sim);

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a team arrival event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_team_arrival(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int idx = find_team_index_by_id(sim, event->id);
    SOFT_ASSERT(idx >= 0, "team for arrival event not found", 0);

    Team *team = &sim->teams[idx];
    SOFT_ASSERT(team->state == TEAM_EN_ROUTE, "arriving team is not en route", 0);

    int call_idx = find_call_index_by_id(sim, team->call_id);
    SOFT_ASSERT(call_idx >= 0, "assigned call not found", 0);

    Call *call = &sim->calls[call_idx];
    SOFT_ASSERT(call->state == CALL_EN_ROUTE, "call of the arriving team is not en route", 0);

    team->district = call->district;
    team->state = TEAM_AT_SCENE;
    call->state = CALL_IN_SERVICE;
    call->service_start = sim->now;

    log_event(sim, "B%d ARRIVED at %s for CALL #%03d (after %d min of waiting)", team->id,
              sim->districts[team->district].name, call->id, sim->now - call->arrival);
    log_event(sim, "B%d STARTED HELP on CALL #%03d, it will take %d min", team->id, call->id, call->service_duration);

    schedule_event_checked(sim, sim->now + call->service_duration, EV_SERVICE_DONE, team->id);

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a service done event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_service_done(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int idx = find_team_index_by_id(sim, event->id);
    SOFT_ASSERT(idx >= 0, "team for service event not found", 0);

    Team *team = &sim->teams[idx];
    SOFT_ASSERT(team->state == TEAM_AT_SCENE, "team that finished service is not at the scene", 0);

    int call_idx = find_call_index_by_id(sim, team->call_id);
    SOFT_ASSERT(call_idx >= 0, "assigned call not found", 0);

    Call *call = &sim->calls[call_idx];

    call->state = CALL_COMPLETED;
    call->completion_time = sim->now;

    sim->served++;
    sim->total_wait += call->service_start - call->arrival;
    sim->total_service += call->completion_time - call->service_start;

    log_event(sim, "CALL #%03d HELP FINISHED by B%d; total wait=%d min, service=%d min", call->id, team->id,
              call->service_start - call->arrival, call->completion_time - call->service_start);

    team->call_id = NO_CALL;

    int time = -1;
    if (sim->return_to_base && team->district != team->base)
        time = travel_time(sim, team, team->base);

    if (time > 0) {
        team->state = TEAM_RETURNING;

        log_event(sim, "B%d RELEASED; returning to base %s, TIME=%d min", team->id, sim->districts[team->base].name, time);

        schedule_event_checked(sim, sim->now + time, EV_RETURN_DONE, team->id);
    } else {
        team->state = TEAM_AVAILABLE;

        if (sim->return_to_base && team->district != team->base)
            log_event(sim, "B%d RELEASED: no road to base, AVAILABLE in district %s", team->id, sim->districts[team->district].name);
        else
            log_event(sim, "B%d RELEASED and AVAILABLE in district %s", team->id, sim->districts[team->district].name);
    }

    try_dispatch(sim);

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a return done event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_return_done(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int team_idx = find_team_index_by_id(sim, event->id);
    SOFT_ASSERT(team_idx >= 0, "team for return event not found", 0);

    Team *team = &sim->teams[team_idx];
    SOFT_ASSERT(team->state == TEAM_RETURNING, "team is not returning", 0);

    team->district = team->base;
    team->state = TEAM_AVAILABLE;

    log_event(sim, "B%d RETURNED to base %s and is AVAILABLE", team->id, sim->districts[team->base].name);

    try_dispatch(sim);

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a cancellation event (the caller hangs up)
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_cancel(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int call_idx = find_call_index_by_id(sim, event->id);
    if (call_idx < 0)
        return 1; // the call slot was reused by the random generator

    Call *call = &sim->calls[call_idx];

    // a dispatched, completed or already closed call is not cancelled
    if (call->state != CALL_WAITING)
        return 1;

    call->state = CALL_CANCELLED;
    remove_call_deadlines(sim, call->id);

    sim->cancelled++;

    log_event(sim, "CALL #%03d CANCELLED by the caller after %d min of waiting", call->id, call->cancel_after);

    return 1;
}

/**
 * @brief Handles the end of the maximal waiting time of a call
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_wait_timeout(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int call_idx = find_call_index_by_id(sim, event->id);
    if (call_idx < 0)
        return 1; // the call slot was reused by the random generator

    Call *call = &sim->calls[call_idx];

    if (call->state != CALL_WAITING)
        return 1;

    call->state = CALL_UNSERVED;
    remove_call_deadlines(sim, call->id);
    sim->unserved++;

    log_event(sim, "CALL #%03d UNSERVED: maximum waiting time %d min exceeded", call->id, call->max_wait);

    return 1;
}

/**
 * @brief Handles an event
 * @param sim Pointer to the simulation structure
 * @param event The event to handle
 * @return 1 on success, 0 on failure
 */
int handle_event(Simulation *sim, Event event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    switch (event.type)
    {
    case EV_CALL_ARRIVAL:
        return handle_call_arrival(sim, &event);

    case EV_ESCALATION:
        return handle_escalation(sim, &event);

    case EV_TEAM_ARRIVAL:
        return handle_team_arrival(sim, &event);

    case EV_SERVICE_DONE:
        return handle_service_done(sim, &event);

    case EV_RETURN_DONE:
        return handle_return_done(sim, &event);

    case EV_CANCEL:
        return handle_cancel(sim, &event);

    case EV_WAIT_TIMEOUT:
        return handle_wait_timeout(sim, &event);

    default:
        fprintf(stderr, "Unknown event type: %d\n", event.type);
        return 0;
    }
}
