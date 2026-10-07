#include "event_handlers.h"
#include "dispatch.h"
#include "simulation.h"
#include "event_queue.h"
#include "output.h"
#include "utils.h"
#include "utils_extra.h"

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

    int idx = find_call_index_by_id(sim, event->id);
    SOFT_ASSERT(idx >= 0, "call for arrival event not found", 0);

    Call *call = &sim->calls[idx];

    // The call must be pending before its arrival event.
    if (call->state != CALL_PENDING)
        return 1;

    // The call has arrived
    call->state = CALL_WAITING;

    log_event(
        sim,
        "CALL #%03d ARRIVED: district=%s priority=%d spec=%s",
        call->id,
        sim->districts[call->district].name,
        call->priority,
        spec_name(call->spec));

    // Schedule escalation and cancellation events
    if (sim->escalation_enabled && call->escalation_after > 0)
    {
        schedule_event_checked(
            sim,
            call->arrival + call->escalation_after,
            EV_ESCALATION,
            call->id);
    }

    if (call->cancel_after > 0)
    {
        schedule_event_checked(
            sim,
            call->arrival + call->cancel_after,
            EV_CANCEL,
            call->id);
    }

    try_dispatch(sim);

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
    SOFT_ASSERT(idx >= 0, "call for escalation event not found", 0);

    Call *call = &sim->calls[idx];
    if (call->state != CALL_WAITING)
        return 1;

    if (call->priority > 1)
    {
        call->priority--;
        sim->escalations++;

        log_event(sim, "CALL #%03d PRIORITY ESCALATED to %d", call->id, call->priority);

        try_dispatch(sim);
    }

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
    int call_idx = find_call_index_by_id(sim, team->call_id);
    SOFT_ASSERT(call_idx >= 0, "assigned call not found", 0);

    Call *call = &sim->calls[call_idx];

    team->district = call->district;
    team->state = TEAM_AT_SCENE;
    call->state = CALL_IN_SERVICE;
    call->service_start = sim->now;

    log_event(sim, "B%d ARRIVED at %s for CALL #%03d; service started", team->id, sim->districts[team->district].name, call->id);

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
    int call_idx = find_call_index_by_id(sim, team->call_id);
    SOFT_ASSERT(call_idx >= 0, "assigned call not found", 0);

    Call *call = &sim->calls[call_idx];

    call->state = CALL_COMPLETED;
    call->completion_time = sim->now;

    sim->served++;
    sim->total_wait += call->service_start - call->arrival;
    sim->total_service += call->completion_time - call->service_start;

    log_event(sim, "CALL #%03d SERVICE COMPLETED by B%d; total wait=%d min", call->id, team->id, call->service_start - call->arrival);

    team->call_id = 0;

    if (sim->return_to_base && team->district != team->base)
    {
        int time = travel_time(sim, team, team->base);
        SOFT_ASSERT(time >= 0, "team cannot return to base", 0);

        team->state = TEAM_RETURNING;

        log_event(sim, "B%d RELEASED; returning to base %s, TIME=%d min", team->id, sim->districts[team->base].name, time);

        schedule_event_checked(sim, sim->now + time, EV_RETURN_DONE, team->id);
    }
    else
    {
        team->state = TEAM_AVAILABLE;

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

    team->district = team->base;
    team->state = TEAM_AVAILABLE;

    log_event(sim, "B%d RETURNED to base %s and is AVAILABLE", team->id, sim->districts[team->base].name);

    try_dispatch(sim);

    sleep_visual(sim);

    return 1;
}

/**
 * @brief Handles a cancellation event
 * @param sim Pointer to the simulation structure
 * @param event Pointer to the event structure
 * @return 1 on success, 0 on failure
 */
static int handle_cancel(Simulation *sim, const Event *event)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(event, "event is NULL", 0);

    int call_idx = find_call_index_by_id(sim, event->id);
    SOFT_ASSERT(call_idx >= 0, "call for cancellation event not found", 0);

    Call *call = &sim->calls[call_idx];

    if (call->state != CALL_WAITING)
        return 1;

    call->state = CALL_CANCELLED;

    sim->cancelled++;

    log_event(sim, "CALL #%03d CANCELLED by timeout rule (%d min)", call->id, call->cancel_after);

    try_dispatch(sim);

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
        handle_call_arrival(sim, &event);
        break;

    case EV_ESCALATION:
        handle_escalation(sim, &event);
        break;

    case EV_TEAM_ARRIVAL:
        handle_team_arrival(sim, &event);
        break;

    case EV_SERVICE_DONE:
        handle_service_done(sim, &event);
        break;

    case EV_RETURN_DONE:
        handle_return_done(sim, &event);
        break;

    case EV_CANCEL:
        handle_cancel(sim, &event);
        break;

    default:
        fprintf(stderr, "Unknown event type: %d\n", event.type);
        break;
    }

    return 1;
}
