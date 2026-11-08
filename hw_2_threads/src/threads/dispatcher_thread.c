#define _POSIX_C_SOURCE 200809L
#include "thread_internal.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"

/**
 * @brief Thread of the dispatcher
 */
void *dispatcher_thread(void *raw)
{
    Simulation *sim = raw;
    set_thread_tag("DSP");

    int dirty = 0;           // something has changed since the last round
    int new_calls[MAX_CALLS];
    int new_count = 0;       // calls that came during the current minute
    Assignment plan[MAX_TEAMS];

    for (;;)
    {
        Message message = mailbox_get(&sim->dispatcher_box);

        if (message.type == MSG_SHUTDOWN) // not counted in Quiet
            break;

        switch (message.type)
        {
        case MSG_NEW_CALL:
            if (new_count < MAX_CALLS)
                new_calls[new_count++] = message.arg;
            dirty = 1;
            break;

        case MSG_CALL_CHANGED:
        case MSG_TEAM_FREE:
            dirty = 1;
            break;

        case MSG_ROUND:
        {
            int made = 0;

            if (dirty)
            {
                pthread_mutex_lock(&sim->world);

                made = plan_dispatch(sim, plan, MAX_TEAMS);

                for (int idx = 0; idx < new_count; idx++)
                {
                    const Call *call = &sim->calls[new_calls[idx]];

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

                pthread_mutex_unlock(&sim->world);
            }

            // the teams start moving by themselves after the order
            for (int idx = 0; idx < made; idx++)
                send_message(sim, &sim->teams[plan[idx].team].mailbox, MSG_ASSIGN, 0, plan[idx].call);

            dirty = 0;
            new_count = 0;
            break;
        }

        default:
            break;
        }

        quiet_done(&sim->quiet);
    }

    return NULL;
}

