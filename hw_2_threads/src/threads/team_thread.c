#define _POSIX_C_SOURCE 200809L
#include "thread_internal.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "event_queue.h"
#include "simulation.h"


/**
 * @brief Thread of the team
 */
void *team_thread(void *raw)
{
    ThreadArgs args = *(ThreadArgs *)raw;
    free(raw);

    Simulation *sim = args.sim;
    Team *team = &sim->teams[args.index];

    char tag[16];
    pthread_mutex_lock(&sim->world);
    snprintf(tag, sizeof(tag), "B%d", team->id);
    pthread_mutex_unlock(&sim->world);
    set_thread_tag(tag);

    for (;;)
    {
        Message message = mailbox_get(&team->mailbox);

        if (message.type == MSG_SHUTDOWN) // not counted in Quiet
            break;

        int finished_call = -1; // index of the call whose thread must be told "help finished"
        int became_free = 0;

        pthread_mutex_lock(&sim->world);

        if (message.type == MSG_ASSIGN)
        {
            // the dispatcher has already reserved the team (state EN_ROUTE)
            Call *call = &sim->calls[message.arg];
            int time = travel_time(sim, team, call->district);

            log_event(sim, "B%d DEPARTED from %s to %s for CALL #%03d, travel TIME=%d min", team->id,
                      sim->districts[team->district].name, sim->districts[call->district].name, call->id, time);

            schedule_event_checked(sim, sim->now + time, EV_TEAM_ARRIVAL, team->id);
        }
        else if (message.type == MSG_WAKE && message.event == EV_TEAM_ARRIVAL)
        {
            int call_idx = find_call_index_by_id(sim, team->call_id);

            if (team->state == TEAM_EN_ROUTE && call_idx >= 0 && sim->calls[call_idx].state == CALL_EN_ROUTE)
            {
                Call *call = &sim->calls[call_idx];

                team->district = call->district;
                team->state = TEAM_AT_SCENE;
                call->state = CALL_IN_SERVICE;
                call->service_start = sim->now;

                log_event(sim, "B%d ARRIVED at %s for CALL #%03d (after %d min of waiting)", team->id,
                          sim->districts[team->district].name, call->id, sim->now - call->arrival);
                log_event(sim, "B%d STARTED HELP on CALL #%03d, it will take %d min", team->id, call->id,
                          call->service_duration);

                schedule_event_checked(sim, sim->now + call->service_duration, EV_SERVICE_DONE, team->id);
            }
            else
                log_event(sim, "ERROR: B%d arrived in a wrong state", team->id);
        }
        else if (message.type == MSG_WAKE && message.event == EV_SERVICE_DONE)
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

                int time = -1;
                if (sim->return_to_base && team->district != team->base)
                    time = travel_time(sim, team, team->base);

                if (time > 0)
                {
                    team->state = TEAM_RETURNING;
                    log_event(sim, "B%d RELEASED; returning to base %s, TIME=%d min", team->id,
                              sim->districts[team->base].name, time);
                    schedule_event_checked(sim, sim->now + time, EV_RETURN_DONE, team->id);
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
        else if (message.type == MSG_WAKE && message.event == EV_RETURN_DONE)
        {
            if (team->state == TEAM_RETURNING)
            {
                team->district = team->base;
                team->state = TEAM_AVAILABLE;
                became_free = 1;

                log_event(sim, "B%d RETURNED to base %s and is AVAILABLE", team->id, sim->districts[team->base].name);
            }
            else
                log_event(sim, "ERROR: B%d returned in a wrong state", team->id);
        }

        pthread_mutex_unlock(&sim->world);

        // messages are sent only after the world mutex is released
        if (finished_call >= 0)
            send_message(sim, &sim->calls[finished_call].mailbox, MSG_CALL_FINISHED, 0, args.index);

        if (became_free)
            send_message(sim, &sim->dispatcher_box, MSG_TEAM_FREE, 0, args.index);

        quiet_done(&sim->quiet);
    }

    return NULL;
}

