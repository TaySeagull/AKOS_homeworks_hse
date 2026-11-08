#define _POSIX_C_SOURCE 200809L
#include "thread_internal.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_queue.h"
#include "simulation.h"

/**
 * @brief Reaction of a call to one of its own events (world mutex is locked)
 * @param sim Pointer to the simulation structure
 * @param call The call
 * @param event Type of the event
 * @param notify Out: message for the dispatcher (-1 = none)
 * @return 1 if the call has been closed by this event
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
            schedule_event_checked(sim, call->arrival + call->escalation_after, EV_ESCALATION, call->id);

        if (call->cancel_after > 0)
            schedule_event_checked(sim, call->arrival + call->cancel_after, EV_CANCEL, call->id);

        schedule_event_checked(sim, call->arrival + call->max_wait, EV_WAIT_TIMEOUT, call->id);

        *notify = MSG_NEW_CALL;
        return 0;

    case EV_ESCALATION:
        if (call->state != CALL_WAITING || call->priority <= 1)
            return 0;

        call->priority--;
        sim->escalations++;

        log_event(sim, "CALL #%03d PRIORITY ESCALATED %d -> %d after %d min of waiting", call->id,
                  call->priority + 1, call->priority, sim->now - call->arrival);

        if (call->priority > 1)
            schedule_event_checked(sim, sim->now + call->escalation_after, EV_ESCALATION, call->id);

        *notify = MSG_CALL_CHANGED;
        return 0;

    case EV_CANCEL:
        if (call->state != CALL_WAITING)
            return 0;

        call->state = CALL_CANCELLED;
        remove_call_deadlines(sim, call->id);
        sim->cancelled++;

        log_event(sim, "CALL #%03d CANCELLED by the caller after %d min of waiting", call->id, call->cancel_after);
        return 1;

    case EV_WAIT_TIMEOUT:
        if (call->state != CALL_WAITING)
            return 0;

        call->state = CALL_UNSERVED;
        remove_call_deadlines(sim, call->id);
        sim->unserved++;

        log_event(sim, "CALL #%03d UNSERVED: maximum waiting time %d min exceeded", call->id, call->max_wait);
        return 1;

    default:
        log_event(sim, "ERROR: CALL #%03d got an unexpected event %d", call->id, event);
        return 0;
    }
}

/**
 * @brief Thread of the call
 */
void *call_thread(void *raw)
{
    ThreadArgs args = *(ThreadArgs *)raw;
    free(raw);

    Simulation *sim = args.sim;
    Call *call = &sim->calls[args.index];

    char tag[16];
    pthread_mutex_lock(&sim->world);
    snprintf(tag, sizeof(tag), "C%03d", call->id);
    pthread_mutex_unlock(&sim->world);
    set_thread_tag(tag);

    for (;;)
    {
        Message message = mailbox_get(&call->mailbox);

        if (message.type == MSG_SHUTDOWN) // not counted in Quiet, see send_shutdown()
            break;

        int notify = -1;

        if (message.type == MSG_WAKE)
        {
            pthread_mutex_lock(&sim->world);
            call_on_event(sim, call, message.event, &notify); // a closed call ignores late wake-ups
            pthread_mutex_unlock(&sim->world);
        }
        // MSG_CALL_FINISHED: the help is finished, nothing else to do for the caller

        if (notify >= 0)
            send_message(sim, &sim->dispatcher_box, (MsgType)notify, 0, args.index);

        quiet_done(&sim->quiet); // must be the last action of the reaction
    }

    return NULL;
}

