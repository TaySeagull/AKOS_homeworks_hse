/**
 * @file threads.h - 2d hw
 */
#ifndef THREADS_H
#define THREADS_H

#include "model.h"

// struct for team thread arguments
typedef struct
{
    Simulation *sim;
    Team *team;
} TeamThreadArgs;

//initialise mutex and sem
int init_thread_sync(Simulation *sim);
// kills thread
void destroy_thread_sync(Simulation *sim);

// start team thread
int start_team_threads(Simulation *sim);
// stop team thread
void stop_team_threads(Simulation *sim);
int notify_team_thread(Simulation *sim, Team *team, TeamCommand command, int district);

#endif